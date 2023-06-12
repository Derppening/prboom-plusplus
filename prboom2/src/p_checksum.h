#ifdef __cplusplus
extern "C" {
#endif  // __cplusplus

extern void (*P_Checksum)(int);
extern void P_ChecksumFinal(void);
void P_RecordChecksum(const char *file);
//void P_VerifyChecksum(const char *file);

#ifdef __cplusplus
extern "C" {
#endif  // __cplusplus
