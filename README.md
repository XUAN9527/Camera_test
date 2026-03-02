## 新增修改

### 增加docker IDF5.4环境，修改idf_component.yml适配。

1. 安装 `Docker`,拉取 `ESP-IDF v5.4` 官方镜像
``` bash
docker pull espressif/idf:release-v5.4
```

2. 启动 `Docker` 并进入工程目录
``` bash
docker run -it --rm \
  --device=/dev/ttyUSB0 \
  -v ~/esp/work:/work \
  -v ~/.ssh:/root/.ssh:ro \
  -w /work/camera_test \
  espressif/idf:release-v5.4 \
  bash -c "git config --global --add safe.directory /work/camera_test && bash"
```

3. 进入`Docker`环境并打开目录编译烧录
``` bash
idf.py set-target esp32s3
idf.py menuconfig
idf.py build
idf.py -p /dev/ttyUSB0 -b 1152000 flash && idf.py -p /dev/ttyUSB0 monitor
```

**备注：** `/dev/ttyUSB0` 根据实际情况修改。