// EmbeddedFont.h
#pragma once

// 这里假设你已经用 imgui 自带工具把某个中文字体（例如 NotoSansSC-Regular.otf）
// 压成了 ImGui 支持的压缩字节数组。
// 实际使用时，把下面这两个变量替换成你自己的数据和大小。

extern const unsigned int g_NotoSansSC_compressed_size;
extern const unsigned char g_NotoSansSC_compressed_data[];
