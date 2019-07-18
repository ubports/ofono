#pragma once

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
