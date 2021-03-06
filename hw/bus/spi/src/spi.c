/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

#include <assert.h>
#include "defs/error.h"
#include "hal/hal_gpio.h"
#include "hal/hal_spi.h"
#include "bus/bus.h"
#include "bus/bus_debug.h"
#include "bus/spi.h"

static int
bus_spi_enable(struct bus_dev *bdev)
{
    struct bus_spi_dev *dev = (struct bus_spi_dev *)bdev;
    int rc;

    BUS_DEBUG_VERIFY_DEV(dev);

    rc = hal_spi_enable(dev->cfg.spi_num);
    if (rc) {
        return SYS_EINVAL;
    }

    return 0;
}

static int
bus_spi_configure(struct bus_dev *bdev, struct bus_node *bnode)
{
    struct bus_spi_dev *dev = (struct bus_spi_dev *)bdev;
    struct bus_spi_node *node = (struct bus_spi_node *)bnode;
    struct bus_spi_node *current_node = (struct bus_spi_node *)bdev->configured_for;
    struct hal_spi_settings spi_cfg;
    int rc;

    BUS_DEBUG_VERIFY_DEV(dev);
    BUS_DEBUG_VERIFY_NODE(node);

    /* No need to reconfigure if already configured with the same settings */
    if (current_node && (current_node->mode == node->mode) &&
                        (current_node->data_order == node->data_order) &&
                        (current_node->freq == node->freq)) {
        return 0;
    }

    rc = hal_spi_disable(dev->cfg.spi_num);
    if (rc) {
        goto done;
    }

    spi_cfg.data_mode = node->mode;
    spi_cfg.data_order = node->data_order;
    spi_cfg.baudrate = node->freq;
    /* XXX add support for other word sizes */
    spi_cfg.word_size = HAL_SPI_WORD_SIZE_8BIT;

    rc = hal_spi_config(dev->cfg.spi_num, &spi_cfg);
    if (rc) {
        goto done;
    }

    rc = hal_spi_enable(dev->cfg.spi_num);

done:
    if (rc) {
        rc = SYS_EIO;
    }

    return rc;
}

static int
bus_spi_read(struct bus_dev *bdev, struct bus_node *bnode, uint8_t *buf,
             uint16_t length, os_time_t timeout, uint16_t flags)
{
    struct bus_spi_dev *dev = (struct bus_spi_dev *)bdev;
    struct bus_spi_node *node = (struct bus_spi_node *)bnode;
    uint8_t val;
    int i;
    int rc;

    BUS_DEBUG_VERIFY_DEV(dev);
    BUS_DEBUG_VERIFY_NODE(node);

    rc = 0;

    hal_gpio_write(node->pin_cs, 0);

    for (i = 0; i < length; i++) {
        val = hal_spi_tx_val(dev->cfg.spi_num, 0xAA);
        if (val == 0xFFFF) {
            rc = SYS_EINVAL;
            break;
        }

        buf[i] = val;
    }

    if (rc || !(flags & BUS_F_NOSTOP)) {
        hal_gpio_write(node->pin_cs, 1);
    }

    return rc;
}

static int
bus_spi_write(struct bus_dev *bdev, struct bus_node *bnode, const uint8_t *buf,
              uint16_t length, os_time_t timeout, uint16_t flags)
{
    struct bus_spi_dev *dev = (struct bus_spi_dev *)bdev;
    struct bus_spi_node *node = (struct bus_spi_node *)bnode;
    int rc;

    BUS_DEBUG_VERIFY_DEV(dev);
    BUS_DEBUG_VERIFY_NODE(node);

    hal_gpio_write(node->pin_cs, 0);

    /* XXX update HAL to accept const instead */
    rc = hal_spi_txrx(dev->cfg.spi_num, (uint8_t *)buf, NULL, length);

    if (!(flags & BUS_F_NOSTOP)) {
        hal_gpio_write(node->pin_cs, 1);
    }

    return rc;
}

static int bus_spi_disable(struct bus_dev *bdev)
{
    struct bus_spi_dev *dev = (struct bus_spi_dev *)bdev;
    int rc;

    BUS_DEBUG_VERIFY_DEV(dev);

    rc = hal_spi_disable(dev->cfg.spi_num);
    if (rc) {
        return SYS_EINVAL;
    }

    return 0;
}

static const struct bus_dev_ops bus_spi_ops = {
    .enable = bus_spi_enable,
    .configure = bus_spi_configure,
    .read = bus_spi_read,
    .write = bus_spi_write,
    .disable = bus_spi_disable,
};

int
bus_spi_dev_init_func(struct os_dev *odev, void *arg)
{
    struct bus_spi_dev *dev = (struct bus_spi_dev *)odev;
    struct bus_spi_dev_cfg *cfg = arg;
    struct hal_spi_hw_settings hal_cfg;
    int rc;

    hal_cfg.pin_sck = cfg->pin_sck;
    hal_cfg.pin_mosi = cfg->pin_mosi;
    hal_cfg.pin_miso = cfg->pin_miso;
    hal_cfg.pin_ss = 0;

    /* XXX we support master only! */
    rc = hal_spi_init_hw(cfg->spi_num, HAL_SPI_TYPE_MASTER, &hal_cfg);
    if (rc) {
        return SYS_EINVAL;
    }

    BUS_DEBUG_POISON_DEV(dev);

    rc = bus_dev_init_func(odev, (void*)&bus_spi_ops);
    assert(rc == 0);

    dev->cfg = *cfg;

    rc = hal_spi_enable(dev->cfg.spi_num);
    assert(rc == 0);

    return 0;
}

int
bus_spi_node_init_func(struct os_dev *odev, void *arg)
{
    struct bus_spi_node *node = (struct bus_spi_node *)odev;
    struct bus_spi_node_cfg *cfg = arg;
    struct bus_node_cfg *node_cfg = &cfg->node_cfg;
    int rc;

    BUS_DEBUG_POISON_NODE(node);

    node->pin_cs = cfg->pin_cs;
    node->mode = cfg->mode;
    node->data_order = cfg->data_order;
    node->freq = cfg->freq;
    node->quirks = cfg->quirks;

    hal_gpio_init_out(node->pin_cs, 1);

    rc = bus_node_init_func(odev, node_cfg);
    if (rc) {
        return rc;
    }

    return 0;
}
