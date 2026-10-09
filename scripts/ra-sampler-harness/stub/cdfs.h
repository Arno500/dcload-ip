#ifndef STUB_CDFS_H
#define STUB_CDFS_H
void gd_exchange(void (*fn)(void));
unsigned int *gd_ra_scratch(unsigned int *words, int *ce);
unsigned int gd_ra_katana_words(void);
unsigned int gd_ra_ce_words(void);
#endif
