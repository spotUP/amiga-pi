/* radius -- the Radius gateways' model catalogs (plan L3; pi 1.1.0 ai/src/providers/radius.ts refreshModels, radius-config.ts).
 *
 * Every Radius provider (the public "radius", and each models.json provider with "oauth":"radius") has a catalog that is the
 * account's, not the shipped one: restored with no network at every start from models-store.json in the agent directory (pi's
 * file and layout: {"<providerId>":{"models":[...],"checkedAt":<ms>}}), and replaced by GET <gateway>/v1/config when there is a
 * credential and the network may be used: at the start of the interactive and RPC modes, on /model, after /login. A custom
 * gateway has no shipped catalog. The pure parts (config to models, replacing a provider's models) are vtcon's llm/radius.h.
 * Portable C89, host-tested (tests/test_pi_radius.c). */
#ifndef PI_RADIUS_H
#define PI_RADIUS_H

#include "login.h"

#define PI_MODELS_STORE "models-store.json"

/* the catalogs restored: models-store.json, else the gatewayConfig an older pi kept in the provider's OAuth entry of auth.json
 * (radius.ts :59-73); a custom gateway with neither has no models. Called by pi_models_init, after the gateways are known and
 * before models.json is laid over the registry. */
void pi_radius_restore(pi_models *m);

/* OpenRouter (P12-A10, pi 1.2.0 personalizeOpenRouterCatalog): the same refresh and the same store: GET <baseUrl>/models/user with the key
 * narrows the shipped catalog to the chat models the key may use (limits and prices from the answer, read as a stream); 401/403/404 keep
 * it; kept in models-store.json under "openrouter" and restored with no network by pi_radius_restore. pi_radius_refresh does it with the
 * Radius ones (only 0 or "openrouter").
 *
 * GET <gateway>/v1/config for the Radius providers that have a credential (only: that provider id), the catalog replaced and
 * saved. Plain http gateways are skipped (a credential never goes over plain http). Returns how many catalogs were refreshed;
 * a failure is the words in err (the last one; "" none) and leaves the provider's models as they are. */
int pi_radius_refresh(pi_auth *a, const char *only, char *err, long cap);

#endif
