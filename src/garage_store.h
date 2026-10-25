#pragma once

#include "cairn_garage.h"

/*
 * The garage in NVS (lib/cairn_garage). Written by provisioning and when a slot learns
 * its car's VIN; read at boot.
 */
bool garage_load(cairn_garage_t *g);        /* false when none is provisioned */
bool garage_save(const cairn_garage_t *g);
