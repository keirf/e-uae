
int scp_open(const char *name, unsigned int drv);
void scp_close(unsigned int drv);
int scp_loadtrack(
    uae_u16 *mfmbuf, uae_u16 *tracktiming, unsigned int drv,
    unsigned int track, unsigned int *tracklength, int *multirev,
    unsigned int *gapoffset);
void scp_loadrevolution(
    uae_u16 *mfmbuf, unsigned int drv, uae_u16 *tracktiming,
    unsigned int *tracklength);
