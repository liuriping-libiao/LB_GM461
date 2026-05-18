// glibc 2.36 兼容层 - 为 glibc < 2.36 的系统提供 arc4random_buf
// 目标设备 (Ubuntu 22.04) 只有 glibc 2.35，缺少 arc4random_buf
#include <sys/random.h>
#include <stddef.h>

// 弱符号：如果系统已有该函数则使用系统版本
__attribute__((weak))
void arc4random_buf(void *buf, size_t n) {
    // getrandom() 从 Linux 3.17 / glibc 2.25 开始可用
    while (n > 0) {
        ssize_t ret = getrandom(buf, n, 0);
        if (ret < 0) continue;  // EINTR 重试
        buf = (char *)buf + ret;
        n -= (size_t)ret;
    }
}
