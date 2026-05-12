#ifndef ARDUINO_COMPAT_H
#define ARDUINO_COMPAT_H

/**
 * Arduino ESP32 兼容层
 * 这里只放 C 可见声明
 */

#ifdef ARDUINO_PLATFORM

#include <stdint.h>
#include <sys/types.h>   // ssize_t

// errno 定义补齐
#ifndef EAGAIN
  #define EAGAIN      11
#endif
#ifndef EWOULDBLOCK
  #define EWOULDBLOCK EAGAIN
#endif
#ifndef ECONNRESET
  #define ECONNRESET  104
#endif
#ifndef EBADF
  #define EBADF       9
#endif
#ifndef EINTR
  #define EINTR       4
#endif

// MSG 标志位
#ifndef MSG_PEEK
  #define MSG_PEEK    0x02
#endif
#ifndef MSG_NOSIGNAL
  #define MSG_NOSIGNAL 0
#endif

// C 声明
#ifdef __cplusplus
extern "C" {
#endif

ssize_t arduino_recv(int fd, void *buf, size_t len, int flags);
ssize_t arduino_send(int fd, const void *buf, size_t len, int flags);
void    arduino_close_fd(int fd);
int     arduino_is_connected(int fd);
int     arduino_fd_in_use(int fd);
int     arduino_available(int fd);
void    arduino_activity_led_begin(uint8_t pin, uint8_t active_level);
void    arduino_activity_led_set_enabled(int enabled);
int64_t arduino_get_time(void);
void    arduino_task_yield(void);
void    arduino_server_begin(void);
int     arduino_accept(void);

#ifdef __cplusplus
}
#endif

// 先引入 lwip 网络定义
#include <lwip/inet.h>

// 系统接口改走兼容层
#ifdef close
  #undef close
#endif
#define close(fd)  arduino_close_fd(fd)

#define recv(fd, buf, len, flags)  arduino_recv(fd, buf, len, flags)
#define send(fd, buf, len, flags)  arduino_send(fd, buf, len, flags)
#define get_program_time           arduino_get_time
#define task_yield()               arduino_task_yield()

// Arduino 下不做这些控制
#define ioctlsocket(...) (0)
#define O_NONBLOCK  0
#define F_GETFL     0
#define F_SETFL     0

#endif // ARDUINO_PLATFORM
#endif // ARDUINO_COMPAT_H
