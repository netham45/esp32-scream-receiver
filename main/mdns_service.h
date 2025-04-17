#ifndef MDNS_SERVICE_H
#define MDNS_SERVICE_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initializes and starts the mDNS service.
 *
 * Sets up the mDNS service with the delegate hostname "_sink._screamrouter" and advertises
 * the "_scream._udp" service on port 4010 (Scream data port).
 * Includes TXT records for service type and potentially audio settings later.
 */
void mdns_service_start(void);

/**
 * @brief Stops the mDNS service.
 */
void mdns_service_stop(void);

#ifdef __cplusplus
}
#endif

#endif // MDNS_SERVICE_H
