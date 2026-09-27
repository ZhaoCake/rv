# 阅读example

首先阅读example.c，非常简单的结构。

大体来说，就是初始化了mem和cpu，用rv_init来作初始化，需要传入机器和内存状态（的指针）以及总线的函数指针。接着把构造的program复制给rv的mem。

其中不理解的是bus的作用。

bus_cb是rv访问外部世界的唯一通道。
