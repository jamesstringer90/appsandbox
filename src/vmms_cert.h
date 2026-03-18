#ifndef VMMS_CERT_H
#define VMMS_CERT_H

#include <windows.h>

/* Check if a VMMS-compatible certificate exists in the machine store. */
BOOL vmms_cert_exists(void);

/* Ensure a VMMS certificate exists for HCS integration services.
   Creates a self-signed cert with the Microsoft VMMS OID if missing.
   Grants the VIRTUAL_MACHINE SID read access to the private key.
   Returns TRUE if a cert is available (existing or newly created). */
BOOL vmms_cert_ensure(void);

#endif /* VMMS_CERT_H */
