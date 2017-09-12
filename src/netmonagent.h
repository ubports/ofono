struct netmon_agent;

struct netmon_agent *netmon_agent_new(const char *path, const char *sender);

void netmon_agent_free(struct netmon_agent *agent);

void netmon_agent_set_removed_notify(struct netmon_agent *agent,
					ofono_destroy_func removed_cb,
					void *user_data);

ofono_bool_t netmon_agent_matches(struct netmon_agent *agent,
				const char *path, const char *sender);

ofono_bool_t netmon_agent_sender_matches(struct netmon_agent *agent,
					const char *sender);

DBusMessage *netmon_agent_new_method_call(struct netmon_agent *netmon,
					const char *method);

void netmon_agent_send_no_reply(struct netmon_agent *agent,
				DBusMessage *message);
