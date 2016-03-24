/* Identities */

INSERT INTO identities (
  type, data
) VALUES ( /* 192.168.0.1 */
  1 , X'c0a80001'
 );

INSERT INTO identities (
  type, data
) VALUES ( /* 192.168.0.100 */
  1 , X'c0a80064'
 );

INSERT INTO identities (
  type, data
) VALUES ( /* 192.168.0.200 */
  1 , X'c0a800c8'
 );

INSERT INTO identities (
  type, data
) VALUES ( /* %any */
  0, '%any'
);

/* Shared Secrets */

INSERT INTO shared_secrets (
   type, data
) VALUES ( 
  1, X'16964066a10de938bdb2ab7864fe4459cab1'
);

INSERT INTO shared_secrets (
   type, data
) VALUES ( 
  1, X'8d5cce342174da772c8224a59885deaa118d'
);

INSERT INTO shared_secret_identity (
  shared_secret, identity
) VALUES (
  1, 1
);

INSERT INTO shared_secret_identity (
  shared_secret, identity
) VALUES (
  1, 2
);

INSERT INTO shared_secret_identity (
  shared_secret, identity
) VALUES (
  2, 1
);

INSERT INTO shared_secret_identity (
  shared_secret, identity
) VALUES (
  2, 3
);

/* Configurations */

INSERT INTO ike_configs (
  local, remote
) VALUES (
  'PH_IP_MOON', '0.0.0.0'
);

INSERT INTO peer_configs (
  name, ike_cfg, local_id, remote_id, auth_method
) VALUES (
  'rw', 1, 1, 4, 2
);

INSERT INTO child_configs (
  name, updown
) VALUES (
  'rw', '/usr/local/libexec/ipsec/_updown iptables'
);

INSERT INTO peer_config_child_config (
  peer_cfg, child_cfg
) VALUES (
  1, 1
);

INSERT INTO traffic_selectors (
  type, start_addr, end_addr
) VALUES ( /* 10.1.0.0/16 */
  7, X'0a010000', X'0a01ffff'
);

INSERT INTO traffic_selectors (
  type
) VALUES ( /* dynamic/32 */
  7
);

INSERT INTO child_config_traffic_selector (
  child_cfg, traffic_selector, kind
) VALUES (
  1, 1, 0
);

INSERT INTO child_config_traffic_selector (
	child_cfg, traffic_selector, kind
) VALUES (
  1, 2, 3
);

