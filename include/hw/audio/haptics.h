/*
 * The taptic engine's drive signal, condensed for an embedder.
 *
 * Copyright (c) 2026 Inferno iOS port.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

#pragma once

#include "qemu/osdep.h"

/*
 * Samples the guest sent its taptic engine: signed 32-bit little-endian mono at 48 kHz. A feed of no
 * bytes says the stream had nothing this time round, and the actuator counts as still from there.
 */
void apple_haptics_feed(const uint8_t* data, size_t len);
