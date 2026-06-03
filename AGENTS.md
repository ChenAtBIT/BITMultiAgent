# 项目
请直接在本项目执行编译和测试。

## 注释风格规范
1. 函数注释：
```cpp
/**
 * @brief 这是一个函数的注释
 * @param param1 这是第一个参数的描述
 * @param param2 这是第二个参数的描述
 * @return 这是返回值的描述
 */
int myFunction(int param1, std::string param2) {
    // 函数体
}
```

2. 代码块注释：
```cpp
// 这是一个代码块的注释，描述这个代码块的功能
if (condition) {
    // 这是一个条件分支的注释，描述这个分支的功能
    doSomething();
} else {
    // 这是另一个条件分支的注释，描述这个分支的功能
    doSomethingElse();
}
```

3. 要对关键代码行进行注释，使用中文注释。

# 编译
- 请直接在本项目执行编译（不要在沙箱内）
- 1. 编译主项目
```bash
# 配置并全量编译
cmake -S . -B build
cmake --build build -j$(nproc)

# 单独编译
cmake --build build --target ai_orchestrator -j$(nproc) 
```

