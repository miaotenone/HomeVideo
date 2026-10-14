# vendored 依赖

只放 header-only 的第三方头文件，这样开发机与 V831（musl 交叉）两侧都无需包管理器。

| 文件 | 版本 | 来源 | 许可 |
| --- | --- | --- | --- |
| `httplib.h` | v0.15.3 | https://github.com/yhirose/cpp-httplib | MIT |
| `json.hpp` | v3.11.3 | https://github.com/nlohmann/json | MIT |

固定下载地址（便于复现）：

```
https://raw.githubusercontent.com/yhirose/cpp-httplib/v0.15.3/httplib.h
https://raw.githubusercontent.com/nlohmann/json/v3.11.3/single_include/nlohmann/json.hpp
```

校验值（sha256）：

```
a3347656c71c81c3fe510fdb1ff229d7db52eb7df04d3342d340eba0b6ad50ea  httplib.h
9bea4c8066ef4a1c206b2be5a36302f8926f7fdc6087af5d20b417d0cf103ea6  json.hpp
```
