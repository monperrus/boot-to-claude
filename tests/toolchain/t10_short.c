/* uint16_t load/store, in an array - the pattern kernel/ata.c uses to
 * read/write ATA sector buffers 16 bits at a time. A doubled store width
 * here would corrupt every other element (or overrun the buffer). */
int main(void) {
    unsigned short buf[4];
    for (int i = 0; i < 4; i++) {
        buf[i] = (unsigned short)(0x1000 + i);
    }
    unsigned short sentinel = 0xBEEF;
    int total = 0;
    for (int i = 0; i < 4; i++) {
        total += buf[i];
    }
    return (total & 0xff) + (sentinel != 0xBEEF ? 100 : 0);
    /* buf = 0x1000,0x1001,0x1002,0x1003 -> sum = 0x4006 = 16390, &0xff = 6 */
}
