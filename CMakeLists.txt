cmake_minimum_required(VERSION 3.0)
project(MyProject)

# 添加可执行文件并指定源文件
add_executable(smart main.cpp)

# 链接所需的库文件
target_link_libraries(smart
    pthread   # 链接 pthread 库
    ibverbs   # 链接 ibverbs 库
    mlx5      # 链接 mlx5 库
)