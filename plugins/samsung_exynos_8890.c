#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

// TODO - From Florian's plugin, why ?
#define OFONO_API_SUBJECT_TO_CHANGE

#include <stddef.h>

#include <ofono/plugin.h>
#include <ofono/log.h>
#include <ofono/modem.h>

#include "ofono.h"

#include "drivers/rilmodem/rilmodem.h"
#include "drivers/rilmodem/vendor.h"
#include "gril.h"
#include "ril.h"

static int samsung_exynos_8890_probe(struct ofono_modem *modem)
{
        return ril_create(modem, OFONO_RIL_VENDOR_SAMSUNG_EXYNOS_8890,
                                NULL,
                                NULL,
                                NULL);
}

static struct ofono_modem_driver samsung_exynos_8890_driver = {
                .name = "samsung_exy_8890",
                .probe = samsung_exynos_8890_probe,
                .remove = ril_remove,
                .enable = ril_enable,
                .disable = ril_disable,
                .pre_sim = ril_pre_sim,
                .post_sim = ril_post_sim,
                .post_online = ril_post_online,
                .set_online = ril_set_online,
};

/*
 * This plugin is a device plugin for Samsung's devices with Exynos-8890 baseband
 * that use the RIL interface. The plugin 'rildev' is used to determine which
 * RIL plugin should be loaded based upon an environment variable.
 */
static int samsung_exynos_8890_init(void)
{
        int retval = ofono_modem_driver_register(&samsung_exynos_8890_driver);

        if (retval)
                DBG("ofono_modem_driver_register returned: %d", retval);

        return retval;
}

static void samsung_exynos_8890_exit(void)
{
        DBG(""); // TODO - To be be removed?
        ofono_modem_driver_unregister(&samsung_exynos_8890_driver);
}

OFONO_PLUGIN_DEFINE(
                samsung_exy_8890,
                "Modem driver for Samsung devices based on EXYNOS-8890 baseband",
                VERSION,
                OFONO_PLUGIN_PRIORITY_DEFAULT,
                samsung_exynos_8890_init,
                samsung_exynos_8890_exit
)
