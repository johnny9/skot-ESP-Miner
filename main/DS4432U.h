#ifndef DS4432U_H_
#define DS4432U_H_

#include "driver/i2c.h"


#ifdef __cplusplus
extern "C"
{
#endif


esp_err_t i2c_master_init(void);
esp_err_t i2c_master_delete(void);
void DS4432U_read(void);

bool DS4432U_set_vcore(float);

#ifdef __cplusplus
}
#endif

#endif /* DS4432U_H_ */