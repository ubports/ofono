
struct at_netreg_data {
	GAtChat *chat;
	char mcc[OFONO_MAX_MCC_LENGTH + 1];
	char mnc[OFONO_MAX_MNC_LENGTH + 1];
	int signal_index; /* If strength is reported via CIND */
	int signal_min; /* min strength reported via CIND */
	int signal_max; /* max strength reported via CIND */
	int signal_invalid; /* invalid strength reported via CIND */
	int tech;
	struct ofono_network_time time;
	guint nitz_timeout;
	unsigned int vendor;
};

void at_registration_status(struct ofono_netreg *netreg,
					ofono_netreg_status_cb_t cb,
					void *data);
void at_current_operator(struct ofono_netreg *netreg,
				ofono_netreg_operator_cb_t cb, void *data);
void at_list_operators(struct ofono_netreg *netreg,
				ofono_netreg_operator_list_cb_t cb, void *data);
void at_register_auto(struct ofono_netreg *netreg,
				ofono_netreg_register_cb_t cb, void *data);
void at_register_manual(struct ofono_netreg *netreg,
				const char *mcc, const char *mnc,
				ofono_netreg_register_cb_t cb, void *data);
void at_signal_strength(struct ofono_netreg *netreg,
				ofono_netreg_strength_cb_t cb, void *data);
void at_netreg_remove(struct ofono_netreg *netreg);
