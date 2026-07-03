#ifndef NETWORK_INTERFACE_H
#define NETWORK_INTERFACE_H

#include <stdbool.h>

// 定义通用的网络驱动结构体
typedef struct {
  const char *name;
  bool (*init)(void);
  bool (*connect)(const char *target, const char *password);
  void (*disconnect)(void);
  bool (*is_connected)(void);
} network_driver_t;

#endif // NETWORK_INTERFACE_H
