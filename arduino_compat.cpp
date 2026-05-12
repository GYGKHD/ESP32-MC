
#ifdef ARDUINO
#ifndef ARDUINO_PLATFORM
#define ARDUINO_PLATFORM
#endif
#endif

#ifdef ARDUINO_PLATFORM

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiServer.h>
#include <WiFiClient.h>
#include <esp_timer.h>
#include <errno.h>
#include <string.h>

#include "arduino_compat.h"
#ifdef recv
#undef recv
#endif
#ifdef send
#undef send
#endif
#ifdef close
#undef close
#endif

#define ARDUINO_MAX_CLIENTS 16
#define ARDUINO_PEEK_BUF 64

static WiFiServer _server(25565);
static WiFiClient _clients[ARDUINO_MAX_CLIENTS];
static bool _used[ARDUINO_MAX_CLIENTS] = {};
static uint8_t _prefetch[ARDUINO_MAX_CLIENTS][ARDUINO_PEEK_BUF];
static size_t _prefetch_len[ARDUINO_MAX_CLIENTS] = {};
static int8_t _activity_led_pin = -1;
static uint8_t _activity_led_on = HIGH;
static uint8_t _activity_led_off = LOW;
static bool _activity_led_enabled = false;
static int64_t _activity_led_until_us = 0;
static const int64_t ARDUINO_ACTIVITY_LED_PULSE_US = 35000;

static size_t min_size(size_t a, size_t b)
{
  return (a < b) ? a : b;
}

static void activity_led_write(uint8_t level)
{
  if (_activity_led_pin < 0)
    return;
  digitalWrite(_activity_led_pin, level);
}

static void activity_led_pulse(void)
{
  if (!_activity_led_enabled || _activity_led_pin < 0)
    return;
  _activity_led_until_us = esp_timer_get_time() + ARDUINO_ACTIVITY_LED_PULSE_US;
  activity_led_write(_activity_led_on);
}

static void refill_prefetch(int fd, size_t need)
{
  if (fd < 0 || fd >= ARDUINO_MAX_CLIENTS || !_used[fd])
    return;
  WiFiClient &c = _clients[fd];

  while (_prefetch_len[fd] < need && _prefetch_len[fd] < ARDUINO_PEEK_BUF)
  {
    int avail = c.available();
    if (avail <= 0)
      break;

    size_t room = ARDUINO_PEEK_BUF - _prefetch_len[fd];
    size_t remaining = need - _prefetch_len[fd];
    size_t to_read = min_size((size_t)avail, min_size(room, remaining));

    int n = c.read(_prefetch[fd] + _prefetch_len[fd], to_read);
    if (n <= 0)
      break;

    _prefetch_len[fd] += (size_t)n;
  }
}

extern "C"
{

  void arduino_server_begin(void)
  {
    _server.begin();
  }

  void arduino_activity_led_begin(uint8_t pin, uint8_t active_level)
  {
    _activity_led_pin = (int8_t)pin;
    _activity_led_on = active_level ? HIGH : LOW;
    _activity_led_off = active_level ? LOW : HIGH;
    pinMode(pin, OUTPUT);
    activity_led_write(_activity_led_off);
  }

  void arduino_activity_led_set_enabled(int enabled)
  {
    _activity_led_enabled = enabled != 0;
    if (!_activity_led_enabled)
    {
      _activity_led_until_us = 0;
      activity_led_write(_activity_led_off);
    }
  }

  int arduino_accept(void)
  {
    WiFiClient c = _server.accept();
    if (!c)
      return -1;
    for (int i = 0; i < ARDUINO_MAX_CLIENTS; i++)
    {
      if (!_used[i])
      {
        _clients[i] = c;
        _used[i] = true;
        _prefetch_len[i] = 0;
        activity_led_pulse();
        return i;
      }
    }
    c.stop();
    return -1;
  }

  void arduino_close_fd(int fd)
  {
    if (fd < 0 || fd >= ARDUINO_MAX_CLIENTS)
      return;
    _clients[fd].stop();
    _used[fd] = false;
    _prefetch_len[fd] = 0;
  }

  ssize_t arduino_recv(int fd, void *buf, size_t len, int flags)
  {
    if (fd < 0 || fd >= ARDUINO_MAX_CLIENTS || !_used[fd])
    {
      errno = EBADF;
      return -1;
    }
    if (len == 0)
      return 0;

    WiFiClient &c = _clients[fd];

    if (flags & MSG_PEEK)
    {
      refill_prefetch(fd, len);

      if (_prefetch_len[fd] == 0)
      {
        if (!c.connected() && !c.available())
          return 0;
        errno = EAGAIN;
        return -1;
      }

      size_t n = min_size(len, _prefetch_len[fd]);
      memcpy(buf, _prefetch[fd], n);
      if (n > 0)
        activity_led_pulse();
      return (ssize_t)n;
    }

    size_t copied = 0;

    // 先消费 MSG_PEEK 预读缓存
    if (_prefetch_len[fd] > 0)
    {
      size_t from_cache = min_size(len, _prefetch_len[fd]);
      memcpy(buf, _prefetch[fd], from_cache);
      copied = from_cache;

      _prefetch_len[fd] -= from_cache;
      if (_prefetch_len[fd] > 0)
      {
        memmove(_prefetch[fd], _prefetch[fd] + from_cache, _prefetch_len[fd]);
      }
    }

    if (copied == len)
    {
      if (copied > 0)
        activity_led_pulse();
      return (ssize_t)copied;
    }
    if (!c.connected() && !c.available())
    {
      if (copied > 0)
        activity_led_pulse();
      return copied > 0 ? (ssize_t)copied : 0;
    }

    int avail = c.available();
    if (avail <= 0)
    {
      if (copied > 0)
      {
        activity_led_pulse();
        return (ssize_t)copied;
      }
      errno = EAGAIN;
      return -1;
    }

    size_t to_read = min_size(len - copied, (size_t)avail);
    int n = c.read(((uint8_t *)buf) + copied, to_read);

    if (n > 0)
    {
      activity_led_pulse();
      return (ssize_t)(copied + (size_t)n);
    }
    if (copied > 0)
    {
      activity_led_pulse();
      return (ssize_t)copied;
    }
    if (!c.connected() && !c.available())
      return 0;

    errno = EAGAIN;
    return -1;
  }

  ssize_t arduino_send(int fd, const void *buf, size_t len, int flags)
  {
    if (fd < 0 || fd >= ARDUINO_MAX_CLIENTS || !_used[fd])
    {
      errno = EBADF;
      return -1;
    }
    WiFiClient &c = _clients[fd];

    size_t w = c.write((const uint8_t *)buf, len);
    if (w == 0)
    {
      // 高负载下 connected() 可能短暂失真
      // 连接标记和接收缓冲都空了才算断开
      if (!c.connected() && !c.available())
        errno = ECONNRESET;
      else
        errno = EAGAIN;
      return -1;
    }
    activity_led_pulse();
    return (ssize_t)w;
  }

  int arduino_is_connected(int fd)
  {
    if (fd < 0 || fd >= ARDUINO_MAX_CLIENTS || !_used[fd])
      return 0;
    // connected() 可能短暂掉成 false
    // 还有数据就仍算已连接
    return (_clients[fd].connected() || _clients[fd].available()) ? 1 : 0;
  }

  int arduino_fd_in_use(int fd)
  {
    if (fd < 0 || fd >= ARDUINO_MAX_CLIENTS)
      return 0;
    return _used[fd] ? 1 : 0;
  }

  int arduino_available(int fd)
  {
    if (fd < 0 || fd >= ARDUINO_MAX_CLIENTS || !_used[fd])
      return 0;
    return (int)_prefetch_len[fd] + _clients[fd].available();
  }

  int64_t arduino_get_time(void)
  {
    return (int64_t)esp_timer_get_time();
  }

  static int64_t _last_yield = 0;
  void arduino_task_yield(void)
  {
    // 主动让出执行权给 WiFi/lwIP
    vTaskDelay(1);
    _last_yield = esp_timer_get_time();
    if (_activity_led_enabled && _activity_led_until_us != 0 && _last_yield >= _activity_led_until_us)
    {
      _activity_led_until_us = 0;
      activity_led_write(_activity_led_off);
    }
  }

} // extern "C"

#endif // ARDUINO_PLATFORM
