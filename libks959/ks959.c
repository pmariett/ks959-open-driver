ks959_device ks959_open();

int ks959_init(ks959_device dev);

int ks959_send_frame(
    ks959_device dev,
    uint8_t data,
    int len
);

int ks959_recv_frame(
    ks959_device dev,
    uint8_t buffer,
    int maxlen
);