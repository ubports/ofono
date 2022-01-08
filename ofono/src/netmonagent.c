#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdint.h>
#include <string.h>
#include <errno.h>

#include <glib.h>
#include <gdbus.h>

#include "ofono.h"
#include "netmonagent.h"

struct netmon_agent {
	char *path;
	char *bus;
	guint disconnect_watch;
	ofono_destroy_func removed_cb;
	void *removed_data;
};

DBusMessage *netmon_agent_new_method_call(struct netmon_agent *agent,
				const char *method)
{
	DBusMessage *msg = dbus_message_new_method_call(agent->bus,
					agent->path,
					OFONO_NETMON_AGENT_INTERFACE,
					method);

	return msg;
}

void netmon_agent_send_no_reply(struct netmon_agent *agent,
				DBusMessage *message)
{
	DBusConnection *conn = ofono_dbus_get_connection();

	dbus_message_set_no_reply(message, TRUE);

	g_dbus_send_message(conn, message);
}

static inline void netmon_agent_send_release(struct netmon_agent *agent)
{
	DBusMessage *msg = netmon_agent_new_method_call(agent, "Release");

	netmon_agent_send_no_reply(agent, msg);
}

ofono_bool_t netmon_agent_matches(struct netmon_agent *agent,
				const char *path, const char *sender)
{
	return g_str_equal(agent->path, path) &&
			g_str_equal(agent->bus, sender);
}

ofono_bool_t netmon_agent_sender_matches(struct netmon_agent *agent,
					const char *sender)
{
	return g_str_equal(agent->bus, sender);
}

void netmon_agent_set_removed_notify(struct netmon_agent *agent,
					ofono_destroy_func destroy,
					void *user_data)
{
	agent->removed_cb = destroy;
	agent->removed_data = user_data;
}

void netmon_agent_free(struct netmon_agent *agent)
{
	DBusConnection *conn = ofono_dbus_get_connection();

	if (agent == NULL)
		return;

	if (agent->disconnect_watch) {
		netmon_agent_send_release(agent);
		g_dbus_remove_watch(conn, agent->disconnect_watch);
		agent->disconnect_watch = 0;
	}

	if (agent->removed_cb)
		agent->removed_cb(agent->removed_data);

	g_free(agent->path);
	g_free(agent->bus);
	g_free(agent);
}

static void netmon_agent_disconnect_cb(DBusConnection *conn, void *user_data)
{
	struct netmon_agent *agent = user_data;

	ofono_debug("Agent exited without calling UnregisterAgent");

	agent->disconnect_watch = 0;

	netmon_agent_free(agent);
}

struct netmon_agent *netmon_agent_new(const char *path,
						const char *sender)
{
	struct netmon_agent *agent = g_try_new0(struct netmon_agent, 1);
	DBusConnection *conn = ofono_dbus_get_connection();

	if (agent == NULL)
		return NULL;

	agent->bus = g_strdup(sender);
	agent->path = g_strdup(path);

	agent->disconnect_watch = g_dbus_add_disconnect_watch(conn, sender,
						netmon_agent_disconnect_cb,
						agent, NULL);

	return agent;
}
