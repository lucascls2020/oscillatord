#include <stdlib.h>
#include <errno.h>
#include <inttypes.h>
#include <string.h>
#include <stdbool.h>

#include <spi2c.h>

#include "config.h"
#include "log.h"
#include "utils.h"

#include "../oscillator.h"
#include "../oscillator_factory.h"

#define FACTORY_NAME "morion"
#define MORION_SPI_BPW 8
#define MORION_DAC_MIN 0
#define MORION_DAC_MAX ((1 << 16) - 1)

struct morion_oscillator {
	struct oscillator oscillator;
	struct spidev *spi;
};

static unsigned int morion_oscillator_index;

static int morion_oscillator_set_dac(struct oscillator *oscillator,
		uint32_t value)
{
	struct morion_oscillator *morion;
	int ret;

	if (value > UINT16_MAX)
		return -ERANGE;

	uint8_t buf[3] = {
		0,
		(value & 0xFF00) >> 8,
		value & 0xFF
	};

	morion = container_of(oscillator, struct morion_oscillator, oscillator);

	log_debug("%s(%s, %" PRIu32 ")\n", __func__, oscillator->name, value);

	ret = spi_transfer(morion->spi, buf, sizeof(buf), NULL, 0);
	if (ret != 0) {
		ret = -errno;
		log_error("failed spi transfer : %m");
		return ret;
	}

	return 0;
}

static int morion_oscillator_apply_output(struct oscillator *oscillator, struct od_output *output) {
	return morion_oscillator_set_dac(oscillator, output->setpoint);
}

static void morion_oscillator_destroy(struct oscillator **oscillator)
{
	struct oscillator *o;
	struct morion_oscillator *m;

	if (oscillator == NULL || *oscillator == NULL)
		return;

	o = *oscillator;
	m = container_of(o, struct morion_oscillator, oscillator);
	spi_delete(m->spi);
	memset(o, 0, sizeof(*o));
	free(o);
	*oscillator = NULL;
}

static struct oscillator *morion_oscillator_new(struct config *config)
{
	struct morion_oscillator *morion;
	long ret;

	uint8_t spi_num;
	uint8_t spi_sub;
	uint32_t spi_speed;


	struct oscillator *oscillator;

	morion = calloc(1, sizeof(*morion));
	if (morion == NULL)
		return NULL;
	oscillator = &morion->oscillator;

	ret = config_get_uint8_t(config, "morion-spi-num");
	if (ret < 0) {
		log_error("morion-spi-num config key must be provided");
		goto error;
	}
	spi_num = ret;

	ret = config_get_uint8_t(config, "morion-spi-sub");
	if (ret < 0) {
		log_error("morion-spi-sub config key must be provided");
		goto error;
	}
	spi_sub = ret;

	ret = config_get_unsigned_number(config, "morion-spi-speed");
	if (ret < 0) {
		log_error("morion-spi-speed config key must me provided");
		goto error;
	}
	spi_speed = ret;

	morion->spi = spi_new(spi_num, spi_sub, spi_speed, MORION_SPI_BPW);
	if (morion->spi == NULL) {
		ret = -errno;
		log_error("spi_new: %m\n");
		goto error;
	}

	oscillator_factory_init(FACTORY_NAME, oscillator, FACTORY_NAME "-%d",
			morion_oscillator_index);
	morion_oscillator_index++;

	log_info("instantiated " FACTORY_NAME " oscillator on spidev%"
	     PRIu8 ".%" PRIu8, spi_num, spi_sub);

	return oscillator;
error:
	morion_oscillator_destroy(&oscillator);
	errno = -ret;
	return NULL;
}

static const struct oscillator_factory morion_oscillator_factory = {
	.class = {
			.name = FACTORY_NAME,
			.get_ctrl = NULL,
			.save = NULL,
			.get_temp = NULL,
			.apply_output = morion_oscillator_apply_output,
			.dac_min = MORION_DAC_MIN,
			.dac_max = MORION_DAC_MAX,
	},
	.new = morion_oscillator_new,
	.destroy = morion_oscillator_destroy,
};

static void __attribute__((constructor)) morion_oscillator_constructor(void)
{
	int ret;

	ret = oscillator_factory_register(&morion_oscillator_factory);
	if (ret < 0)
		log_error("oscillator_factory_register", ret);
}
