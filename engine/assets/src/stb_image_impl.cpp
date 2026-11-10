// stb_image 的单一定义单元：宏必须恰好在一个 .cpp 里展开一次，
// 否则会产生重复符号。其他文件只包含 <stb_image.h> 使用其声明。
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
