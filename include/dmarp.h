#ifndef DMARP_H
#define DMARP_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "dmod_types.h"
#include "dmarp_defs.h"

/**
 * Public API for the dmarp module.
 *
 * Functions are declared with the dmod_dmarp_api(...) macro - dmod's
 * standard pattern for functions callable from other modules (or from this
 * module's own tests/), resolved dynamically by the loader rather than
 * through normal static linkage. See dm_sw_ring/include/dm_sw_ring.h for a
 * fully worked real-world example of the same shape.
 *
 * Definitions in src/dmarp.c use the matching
 * dmod_dmarp_api_declaration(...) macro - a plain C function
 * definition here will NOT satisfy these declarations at link time.
 *
 * This is an example interface using the usual "opaque handle" pattern -
 * replace the handle, functions, and struct definition in
 * src/dmarp.c with your module's real API.
 */

/* Opaque handle - the real struct is defined in src/dmarp.c */
typedef struct dmarp* dmarp_t;

/**
 * Create a new dmarp instance.
 *
 * @return A valid handle on success, or NULL on allocation failure.
 */
dmod_dmarp_api(1.0, dmarp_t, _create, ( void ));

/**
 * Destroy an instance created by dmarp_create(). Safe to call with
 * NULL.
 */
dmod_dmarp_api(1.0, void, _destroy, ( dmarp_t handle ));

/**
 * Example accessor - replace with your module's real API.
 *
 * @return true if handle is a valid, non-NULL instance.
 */
dmod_dmarp_api(1.0, bool, _is_valid, ( dmarp_t handle ));

#endif // DMARP_H
