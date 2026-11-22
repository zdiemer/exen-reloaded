/* Name hashes (docs/re/CVP.md §5).
 * Class hash  = CRC-32 (zlib) of the dotted class name.
 * Member hash = crc16(JVM descriptor) << 16 | crc16(name), CRC-16/MCRF4XX. */
#include <string.h>

#include "vm.h"

uint32_t exen_crc32(const void *data, size_t len)
{
    const uint8_t *p = data;
    uint32_t c = 0xffffffffu;
    while (len--) {
        c ^= *p++;
        for (int k = 0; k < 8; k++)
            c = (c & 1) ? (c >> 1) ^ 0xedb88320u : c >> 1;
    }
    return c ^ 0xffffffffu;
}

uint16_t exen_crc16(const void *data, size_t len)
{
    const uint8_t *p = data;
    uint16_t c = 0xffff;
    while (len--) {
        c ^= *p++;
        for (int k = 0; k < 8; k++)
            c = (c & 1) ? (uint16_t)((c >> 1) ^ 0x8408) : (uint16_t)(c >> 1);
    }
    return c;
}

uint32_t exen_member_hash(const char *name, const char *desc)
{
    return (uint32_t)exen_crc16(desc, strlen(desc)) << 16 | exen_crc16(name, strlen(name));
}
