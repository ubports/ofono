
#ifndef __OFONO_QMI_VOICE_GENERATED_H
#define __OFONO_QMI_VOICE_GENERATED_H

#include "qmi.h"

struct qmi_voice_remote_party_number_instance {
	uint8_t call_id;
	uint8_t presentation_indicator;
	uint8_t number_size;
	char number[0];
} __attribute__((__packed__));

struct qmi_voice_remote_party_number {
	uint8_t size;
	struct qmi_voice_remote_party_number_instance instance[0];
} __attribute__((__packed__));

/* generator / parser */

struct qmi_voice_dial_call_arg {
	bool calling_number_set;
	const char *calling_number;
	bool call_type_set;
	uint8_t call_type;
};

int qmi_voice_dial_call(
		struct qmi_voice_dial_call_arg *arg,
		struct qmi_service *service,
		qmi_result_func_t func,
		void *user_data,
		qmi_destroy_func_t destroy);

struct qmi_voice_dial_call_result {
	bool call_id_set;
	uint8_t call_id;
};

enum parse_error qmi_voice_dial_call_parse(
		struct qmi_result *qmi_result,
		struct qmi_voice_dial_call_result *result);

struct qmi_voice_end_call_arg {
	bool call_id_set;
	uint8_t call_id;
};

int qmi_voice_end_call(
		struct qmi_voice_end_call_arg *arg,
		struct qmi_service *service,
		qmi_result_func_t func,
		void *user_data,
		qmi_destroy_func_t destroy);

struct qmi_voice_end_call_result {
	bool call_id_set;
	uint8_t call_id;
};

enum parse_error qmi_voice_end_call_parse(
		struct qmi_result *qmi_result,
		struct qmi_voice_end_call_result *result);

struct qmi_voice_answer_call_arg {
	bool call_id_set;
	uint8_t call_id;
};

int qmi_voice_answer_call(
		struct qmi_voice_answer_call_arg *arg,
		struct qmi_service *service,
		qmi_result_func_t func,
		void *user_data,
		qmi_destroy_func_t destroy);

struct qmi_voice_answer_call_result {
	bool call_id_set;
	uint8_t call_id;
};

enum parse_error qmi_voice_answer_call_parse(
		struct qmi_result *qmi_result,
		struct qmi_voice_answer_call_result *result);

struct qmi_voice_call_information_instance {
	uint8_t id;
	uint8_t state;
	uint8_t type;
	uint8_t direction;
	uint8_t mode;
	uint8_t multipart_indicator;
	uint8_t als;
} __attribute__((__packed__));

struct qmi_voice_call_information {
	uint8_t size;
	struct qmi_voice_call_information_instance instance[0];
} __attribute__((__packed__)) ;

struct qmi_voice_all_call_status_ind {
	bool call_information_set;
	const struct qmi_voice_call_information *call_information;
	bool remote_party_number_set;
	uint8_t remote_party_number_size;
	const struct qmi_voice_remote_party_number_instance *remote_party_number[16];
};

enum parse_error qmi_voice_call_status(
		struct qmi_result *qmi_result,
		struct qmi_voice_all_call_status_ind *result);

#endif /* __OFONO_QMI_VOICE_GENERATED_H */
