# 第三方来源与致谢

## usbuart-termux

- 上游：https://github.com/thingsapart/usbuart-termux
- 维护者：thingsapart 及上游贡献者。
- 关系：termux-radio 基于该项目修改，继续使用其面向 Termux 的 libusb 串口访问基础。

## USBUART

- 原始项目：https://github.com/hutorny/usbuart
- 原作者：Eugene Hutorny，源文件中的版权声明为 Copyright (C) 2016 Eugene Hutorny。
- 关系：usbuart-termux 的上游；本仓库保留其库代码、接口及原有版权和许可声明。
- 仓库保留原有 `LICENSE`（GNU GPL version 2 文本）；部分库源文件另有 GNU LGPL v2 声明，具体文件以相应原有声明为准。本说明不替换或删除这些声明。

## Linux CP210x 驱动

- 上游文件：https://github.com/torvalds/linux/blob/master/drivers/usb/serial/cp210x.c
- 上游标识：`SPDX-License-Identifier: GPL-2.0`。
- 上游文件列出的版权作者：Craig Shelley（2005）、Johan Hovold（2010–2021）；同时感谢其他驱动贡献者。
- 对应实现：本项目 `src/cp210x.cpp`，参考 Linux 驱动的 CP210x 控制请求、波特率和串口配置逻辑，适配为 libusb 用户态实现。
- Linux 内核许可证说明：https://github.com/torvalds/linux/blob/master/LICENSES/preferred/GPL-2.0

以上项目的名称及作者归属保留给各自权利人；termux-radio 的新增功能不改变这些上游贡献的来源。
