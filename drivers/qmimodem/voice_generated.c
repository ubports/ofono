
#include <stdint.h>
#include <string.h>
#include <glib.h>

#include "voice_generated.h"

int qmi_voice_dial_call(
		struct qmi_voice_dial_call_arg *arg,
		struct qmi_service *service,
		qmi_result_func_t func,
		void *user_data,
		qmi_destroy_func_t destroy)
{
	struct qmi_param *param = NULL;

	param = qmi_param_new();
	if (!param)
		goto error;

	if (arg->calling_number_set) {
		if (!qmi_param_append(param,
				 0x1,
				 strlen(arg->calling_number),
				 arg->calling_number))
			goto error;
	}

	if (arg->call_type_set)
		qmi_param_append_uint8(param, 0x10, arg->call_type);

	if (qmi_service_send(service,
			     0x20,
			     param,
			     func,
			     user_data,
			     destroy) > 0)
		return 0;
error:
	g_free(param);
	return 1;
}

enum parse_error qmi_voice_dial_call_parse(
		struct qmi_result *qmi_result,
		struct qmi_voice_dial_call_result *result)
{
	int err = NONE;

	/* mandatory */
	if (qmi_result_get_uint8(qmi_result, 0x10, &result->call_id))
		result->call_id_set = 1;
	else
		err = MISSING_MANDATORY;

	return err;
}

int qmi_voice_end_call(
		struct qmi_voice_end_call_arg *arg,
		struct qmi_service *service,
		qmi_result_func_t func,
		void *user_data,
		qmi_destroy_func_t destroy)
{
	struct qmi_param *param = NULL;

	param = qmi_param_new();
	if (!param)
		goto error;

	if (arg->call_id_set) {
		if (!qmi_param_append_uint8(
					param,
					0x1,
					arg->call_id))
			goto error;
	}

	if (qmi_service_send(service,
			     0x21,
			     param,
			     func,
			     user_data,
			     destroy) > 0)
		return 0;
error:
	g_free(param);
	return 1;
}

enum parse_error qmi_voice_end_call_parse(
		struct qmi_result *qmi_result,
		struct qmi_voice_end_call_result *result)
{
	int err = NONE;

	/* optional */
	if (qmi_result_get_uint8(qmi_result, 0x10, &result->call_id))
		result->call_id_set = 1;

	return err;
}


int qmi_voice_answer_call(
		struct qmi_voice_answer_call_arg *arg,
		struct qmi_service *service,
		qmi_result_func_t func,
		void *user_data,
		qmi_destroy_func_t destroy)
{
	struct qmi_param *param = NULL;

	param = qmi_param_new();
	if (!param)
		goto error;

	if (arg->call_id_set) {
		if (!qmi_param_append_uint8(
					param,
					0x1,
					arg->call_id))
			goto error;
	}

	if (qmi_service_send(service,
			     0x22,
			     param,
			     func,
			     user_data,
			     destroy) > 0)
		return 0;
error:
	g_free(param);
	return 1;
}


enum parse_error qmi_voice_answer_call_parse(
		struct qmi_result *qmi_result,
		struct qmi_voice_answer_call_result *result)
{
	int err = NONE;

	/* optional */
	if (qmi_result_get_uint8(qmi_result, 0x10, &result->call_id))
		result->call_id_set = 1;

	return err;
}

enum parse_error qmi_voice_ind_call_status(
		struct qmi_result *qmi_result,
		struct qmi_voice_all_call_status_ind *result)
{
	int err = NONE;
	int offset;
	uint16_t len;
	const struct qmi_voice_remote_party_number *remote_party_number;
	const struct qmi_voice_call_information *call_information;

	/* mandatory */
	call_information = qmi_result_get(qmi_result, 0x01, &len);
	if (call_information)
	{
		/* verify the length */
		if (len < sizeof(call_information->size))
			return INVALID_LENGTH;

		if (len != call_information->size * sizeof(struct qmi_voice_call_information_instance)
			    + sizeof(call_information->size))
			return INVALID_LENGTH;
		result->call_information_set = 1;
		result->call_information = call_information;
	} else
		return MISSING_MANDATORY;

	/* mandatory */
	remote_party_number = qmi_result_get(qmi_result, 0x10, &len);
	if (remote_party_number) {
		const struct qmi_voice_remote_party_number_instance *instance;
		int instance_size = sizeof(struct qmi_voice_remote_party_number_instance);
		int i;

		/* verify the length */
		if (len < sizeof(remote_party_number->size))
			return INVALID_LENGTH;

		for (i = 0, offset = sizeof(remote_party_number->size);
		     offset <= len && i < 16 && i < remote_party_number->size; i++)
		{
			if (offset == len) {
				break;
			} else if (offset + instance_size > len) {
				return INVALID_LENGTH;
			}

			instance = (void *)remote_party_number + offset;
			result->remote_party_number[i] = instance;
			offset += sizeof(struct qmi_voice_remote_party_number_instance) + instance->number_size;
		}
		result->remote_party_number_set = 1;
		result->remote_party_number_size = remote_party_number->size;
	} else
		return MISSING_MANDATORY;

	return err;
}
