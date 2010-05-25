/*
 * Copyright (C) 2006-2009 Martin Willi
 * Hochschule fuer Technik Rapperswil
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.  See <http://www.fsf.org/copyleft/gpl.txt>.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * for more details.
 */

#include "eap_aka_peer.h"

#include <library.h>
#include <daemon.h>

#include <simaka_message.h>
#include <simaka_crypto.h>

typedef struct private_eap_aka_peer_t private_eap_aka_peer_t;

/**
 * Private data of an eap_aka_peer_t object.
 */
struct private_eap_aka_peer_t {

	/**
	 * Public authenticator_t interface.
	 */
	eap_aka_peer_t public;

	/**
	 * EAP-AKA crypto helper
	 */
	simaka_crypto_t *crypto;

	/**
	 * permanent ID of peer
	 */
	identification_t *permanent;

	/**
	 * Pseudonym identity the peer uses
	 */
	identification_t *pseudonym;

	/**
	 * Reauthentication identity the peer uses
	 */
	identification_t *reauth;

	/**
	 * MSK
	 */
	chunk_t msk;

	/**
	 * Master key, if reauthentication is used
	 */
	char mk[HASH_SIZE_SHA1];

	/**
	 * Counter value if reauthentication is used
	 */
	u_int16_t counter;
};

/**
 * Create a AKA_CLIENT_ERROR: "Unable to process"
 */
static eap_payload_t* create_client_error(private_eap_aka_peer_t *this,
										  u_int8_t identifier)
{
	simaka_message_t *message;
	eap_payload_t *out;
	u_int16_t encoded;

	DBG1(DBG_IKE, "sending client error '%N'",
		 simaka_client_error_names, AKA_UNABLE_TO_PROCESS);

	message = simaka_message_create(FALSE, identifier, EAP_AKA,
									AKA_CLIENT_ERROR, this->crypto);
	encoded = htons(AKA_UNABLE_TO_PROCESS);
	message->add_attribute(message, AT_CLIENT_ERROR_CODE,
						   chunk_create((char*)&encoded, sizeof(encoded)));
	out = message->generate(message, chunk_empty);
	message->destroy(message);
	return out;
}

/**
 * process an EAP-AKA/Request/Identity message
 */
static status_t process_identity(private_eap_aka_peer_t *this,
								 simaka_message_t *in, eap_payload_t **out)
{
	simaka_message_t *message;
	enumerator_t *enumerator;
	simaka_attribute_t type;
	chunk_t data, id = chunk_empty;
	simaka_attribute_t id_req = 0;

	/* reset previously uses reauthentication/pseudonym data */
	this->crypto->clear_keys(this->crypto);
	DESTROY_IF(this->pseudonym);
	this->pseudonym = NULL;
	DESTROY_IF(this->reauth);
	this->reauth = NULL;

	enumerator = in->create_attribute_enumerator(in);
	while (enumerator->enumerate(enumerator, &type, &data))
	{
		switch (type)
		{
			case AT_ANY_ID_REQ:
			case AT_FULLAUTH_ID_REQ:
			case AT_PERMANENT_ID_REQ:
				id_req = type;
				break;
			default:
				if (!simaka_attribute_skippable(type))
				{
					*out = create_client_error(this, in->get_identifier(in));
					enumerator->destroy(enumerator);
					return NEED_MORE;
				}
				break;
		}
	}
	enumerator->destroy(enumerator);

	switch (id_req)
	{
		case AT_ANY_ID_REQ:
			this->reauth = charon->sim->card_get_reauth(charon->sim,
									this->permanent, this->mk, &this->counter);
			if (this->reauth)
			{
				id = this->reauth->get_encoding(this->reauth);
				break;
			}
			/* FALL */
		case AT_FULLAUTH_ID_REQ:
			this->pseudonym = charon->sim->card_get_pseudonym(charon->sim,
															  this->permanent);
			if (this->pseudonym)
			{
				id = this->pseudonym->get_encoding(this->pseudonym);
				break;
			}
			/* FALL */
		case AT_PERMANENT_ID_REQ:
			id = this->permanent->get_encoding(this->permanent);
			break;
		default:
			break;
	}
	message = simaka_message_create(FALSE, in->get_identifier(in), EAP_AKA,
									AKA_IDENTITY, this->crypto);
	if (id.len)
	{
		message->add_attribute(message, AT_IDENTITY, id);
	}
	*out = message->generate(message, chunk_empty);
	message->destroy(message);

	return NEED_MORE;
}

/**
 * Process an EAP-AKA/Request/Challenge message
 */
static status_t process_challenge(private_eap_aka_peer_t *this,
								  simaka_message_t *in, eap_payload_t **out)
{
	simaka_message_t *message;
	enumerator_t *enumerator;
	simaka_attribute_t type;
	chunk_t data, rand = chunk_empty, autn = chunk_empty, mk;
	u_char res[AKA_RES_MAX], ck[AKA_CK_LEN], ik[AKA_IK_LEN], auts[AKA_AUTS_LEN];
	int res_len;
	identification_t *id;
	status_t status;

	enumerator = in->create_attribute_enumerator(in);
	while (enumerator->enumerate(enumerator, &type, &data))
	{
		switch (type)
		{
			case AT_RAND:
				rand = data;
				break;
			case AT_AUTN:
				autn = data;
				break;
			default:
				if (!simaka_attribute_skippable(type))
				{
					*out = create_client_error(this, in->get_identifier(in));
					enumerator->destroy(enumerator);
					return NEED_MORE;
				}
				break;
		}
	}
	enumerator->destroy(enumerator);

	if (!rand.len || !autn.len)
	{
		DBG1(DBG_IKE, "received invalid EAP-AKA challenge message");
		*out = create_client_error(this, in->get_identifier(in));
		return NEED_MORE;
	}

	status = charon->sim->card_get_quintuplet(charon->sim, this->permanent,
									rand.ptr, autn.ptr, ck, ik, res, &res_len);
	if (status == INVALID_STATE &&
		charon->sim->card_resync(charon->sim, this->permanent, rand.ptr, auts))
	{
		DBG1(DBG_IKE, "received SQN invalid, sending %N",
			 simaka_subtype_names, AKA_SYNCHRONIZATION_FAILURE);
		message = simaka_message_create(FALSE, in->get_identifier(in), EAP_AKA,
									AKA_SYNCHRONIZATION_FAILURE, this->crypto);
		message->add_attribute(message, AT_AUTS,
							   chunk_create(auts, AKA_AUTS_LEN));
		*out = message->generate(message, chunk_empty);
		message->destroy(message);
		return NEED_MORE;
	}
	if (status != SUCCESS)
	{
		DBG1(DBG_IKE, "no USIM found with quintuplets for '%Y', sending %N",
			 this->permanent, simaka_subtype_names, AKA_AUTHENTICATION_REJECT);
		message = simaka_message_create(FALSE, in->get_identifier(in), EAP_AKA,
										AKA_AUTHENTICATION_REJECT, this->crypto);
		*out = message->generate(message, chunk_empty);
		message->destroy(message);
		return NEED_MORE;
	}

	id = this->permanent;
	if (this->pseudonym)
	{
		id = this->pseudonym;
	}
	data = chunk_cata("cc", chunk_create(ik, AKA_IK_LEN),
					  chunk_create(ck, AKA_CK_LEN));
	free(this->msk.ptr);
	this->msk = this->crypto->derive_keys_full(this->crypto, id, data, &mk);
	memcpy(this->mk, mk.ptr, mk.len);
	free(mk.ptr);

	/* Verify AT_MAC attribute and parse() again after key derivation,
	 * reading encrypted attributes */
	if (!in->verify(in, chunk_empty) || !in->parse(in))
	{
		*out = create_client_error(this, in->get_identifier(in));
		return NEED_MORE;
	}

	enumerator = in->create_attribute_enumerator(in);
	while (enumerator->enumerate(enumerator, &type, &data))
	{
		switch (type)
		{
			case AT_NEXT_REAUTH_ID:
				this->counter = 0;
				id = identification_create_from_data(data);
				charon->sim->card_set_reauth(charon->sim, this->permanent, id,
											 this->mk, this->counter);
				id->destroy(id);
				break;
			case AT_NEXT_PSEUDONYM:
				id = identification_create_from_data(data);
				charon->sim->card_set_pseudonym(charon->sim, this->permanent, id);
				id->destroy(id);
				break;
			default:
				break;
		}
	}
	enumerator->destroy(enumerator);

	message = simaka_message_create(FALSE, in->get_identifier(in), EAP_AKA,
									AKA_CHALLENGE, this->crypto);
	message->add_attribute(message, AT_RES, chunk_create(res, res_len));
	*out = message->generate(message, chunk_empty);
	message->destroy(message);
	return NEED_MORE;
}

/**
 * Check if a received counter value is acceptable
 */
static bool counter_too_small(private_eap_aka_peer_t *this, chunk_t chunk)
{
	u_int16_t counter;

	memcpy(&counter, chunk.ptr, sizeof(counter));
	counter = htons(counter);
	return counter < this->counter;
}

/**
 * process an EAP-AKA/Request/Reauthentication message
 */
static status_t process_reauthentication(private_eap_aka_peer_t *this,
									simaka_message_t *in, eap_payload_t **out)
{
	simaka_message_t *message;
	enumerator_t *enumerator;
	simaka_attribute_t type;
	chunk_t data, counter = chunk_empty, nonce = chunk_empty, id = chunk_empty;

	if (!this->reauth)
	{
		DBG1(DBG_IKE, "received %N, but not expected",
			 simaka_subtype_names, AKA_REAUTHENTICATION);
		*out = create_client_error(this, in->get_identifier(in));
		return NEED_MORE;
	}

	this->crypto->derive_keys_reauth(this->crypto,
									 chunk_create(this->mk, HASH_SIZE_SHA1));

	/* verify MAC and parse again with decryption key */
	if (!in->verify(in, chunk_empty) || !in->parse(in))
	{
		*out = create_client_error(this, in->get_identifier(in));
		return NEED_MORE;
	}

	enumerator = in->create_attribute_enumerator(in);
	while (enumerator->enumerate(enumerator, &type, &data))
	{
		switch (type)
		{
			case AT_COUNTER:
				counter = data;
				break;
			case AT_NONCE_S:
				nonce = data;
				break;
			case AT_NEXT_REAUTH_ID:
				id = data;
				break;
			default:
				if (!simaka_attribute_skippable(type))
				{
					*out = create_client_error(this, in->get_identifier(in));
					enumerator->destroy(enumerator);
					return NEED_MORE;
				}
				break;
		}
	}
	enumerator->destroy(enumerator);

	if (!nonce.len || !counter.len)
	{
		DBG1(DBG_IKE, "EAP-AKA/Request/Reauthentication message incomplete");
		*out = create_client_error(this, in->get_identifier(in));
		return NEED_MORE;
	}

	message = simaka_message_create(FALSE, in->get_identifier(in), EAP_AKA,
									AKA_REAUTHENTICATION, this->crypto);
	if (counter_too_small(this, counter))
	{
		DBG1(DBG_IKE, "reauthentication counter too small");
		message->add_attribute(message, AT_COUNTER_TOO_SMALL, chunk_empty);
	}
	else
	{
		free(this->msk.ptr);
		this->msk = this->crypto->derive_keys_reauth_msk(this->crypto,
										this->reauth, counter, nonce,
										chunk_create(this->mk, HASH_SIZE_SHA1));
		if (id.len)
		{
			identification_t *reauth;

			reauth = identification_create_from_data(data);
			charon->sim->card_set_reauth(charon->sim, this->permanent, reauth,
							 this->mk, this->counter);
			reauth->destroy(reauth);
		}
	}
	message->add_attribute(message, AT_COUNTER, counter);
	*out = message->generate(message, nonce);
	message->destroy(message);
	return NEED_MORE;
}

/**
 * Process an EAP-AKA/Request/Notification message
 */
static status_t process_notification(private_eap_aka_peer_t *this,
									 simaka_message_t *in, eap_payload_t **out)
{
	simaka_message_t *message;
	enumerator_t *enumerator;
	simaka_attribute_t type;
	chunk_t data;
	bool success = TRUE;

	enumerator = in->create_attribute_enumerator(in);
	while (enumerator->enumerate(enumerator, &type, &data))
	{
		if (type == AT_NOTIFICATION)
		{
			u_int16_t code;

			memcpy(&code, data.ptr, sizeof(code));
			code = ntohs(code);

			/* test success bit */
			if (!(data.ptr[0] & 0x80))
			{
				success = FALSE;
				DBG1(DBG_IKE, "received EAP-AKA notification error '%N'",
					 simaka_notification_names, code);
			}
			else
			{
				DBG1(DBG_IKE, "received EAP-AKA notification '%N'",
					 simaka_notification_names, code);
			}
		}
		else if (!simaka_attribute_skippable(type))
		{
			success = FALSE;
			break;
		}
	}
	enumerator->destroy(enumerator);

	if (success)
	{	/* empty notification reply */
		message = simaka_message_create(FALSE, in->get_identifier(in), EAP_AKA,
										AKA_NOTIFICATION, this->crypto);
		*out = message->generate(message, chunk_empty);
		message->destroy(message);
	}
	else
	{
		*out = create_client_error(this, in->get_identifier(in));
	}
	return NEED_MORE;
}


/**
 * Implementation of eap_method_t.process
 */
static status_t process(private_eap_aka_peer_t *this,
						eap_payload_t *in, eap_payload_t **out)
{
	simaka_message_t *message;
	status_t status;

	message = simaka_message_create_from_payload(in, this->crypto);
	if (!message)
	{
		*out = create_client_error(this, in->get_identifier(in));
		return NEED_MORE;
	}
	if (!message->parse(message))
	{
		message->destroy(message);
		*out = create_client_error(this, in->get_identifier(in));
		return NEED_MORE;
	}
	switch (message->get_subtype(message))
	{
		case AKA_IDENTITY:
			status = process_identity(this, message, out);
			break;
		case AKA_CHALLENGE:
			status = process_challenge(this, message, out);
			break;
		case AKA_REAUTHENTICATION:
			status = process_reauthentication(this, message, out);
			break;
		case AKA_NOTIFICATION:
			status = process_notification(this, message, out);
			break;
		default:
			DBG1(DBG_IKE, "unable to process EAP-AKA subtype %N",
				 simaka_subtype_names, message->get_subtype(message));
			*out = create_client_error(this, in->get_identifier(in));
			status = NEED_MORE;
			break;
	}
	message->destroy(message);
	return status;
}

/**
 * Implementation of eap_method_t.initiate
 */
static status_t initiate(private_eap_aka_peer_t *this, eap_payload_t **out)
{
	/* peer never initiates */
	return FAILED;
}

/**
 * Implementation of eap_method_t.get_type.
 */
static eap_type_t get_type(private_eap_aka_peer_t *this, u_int32_t *vendor)
{
	*vendor = 0;
	return EAP_AKA;
}

/**
 * Implementation of eap_method_t.get_msk.
 */
static status_t get_msk(private_eap_aka_peer_t *this, chunk_t *msk)
{
	if (this->msk.ptr)
	{
		*msk = this->msk;
		return SUCCESS;
	}
	return FAILED;
}

/**
 * Implementation of eap_method_t.is_mutual.
 */
static bool is_mutual(private_eap_aka_peer_t *this)
{
	return TRUE;
}

/**
 * Implementation of eap_method_t.destroy.
 */
static void destroy(private_eap_aka_peer_t *this)
{
	this->crypto->destroy(this->crypto);
	this->permanent->destroy(this->permanent);
	DESTROY_IF(this->pseudonym);
	DESTROY_IF(this->reauth);
	free(this->msk.ptr);
	free(this);
}

/*
 * Described in header.
 */
eap_aka_peer_t *eap_aka_peer_create(identification_t *server,
									identification_t *peer)
{
	private_eap_aka_peer_t *this = malloc_thing(private_eap_aka_peer_t);

	this->public.interface.initiate = (status_t(*)(eap_method_t*,eap_payload_t**))initiate;
	this->public.interface.process = (status_t(*)(eap_method_t*,eap_payload_t*,eap_payload_t**))process;
	this->public.interface.get_type = (eap_type_t(*)(eap_method_t*,u_int32_t*))get_type;
	this->public.interface.is_mutual = (bool(*)(eap_method_t*))is_mutual;
	this->public.interface.get_msk = (status_t(*)(eap_method_t*,chunk_t*))get_msk;
	this->public.interface.destroy = (void(*)(eap_method_t*))destroy;

	this->crypto = simaka_crypto_create();
	if (!this->crypto)
	{
		free(this);
		return NULL;
	}
	this->permanent = peer->clone(peer);
	this->pseudonym = NULL;
	this->reauth = NULL;
	this->msk = chunk_empty;

	return &this->public;
}

