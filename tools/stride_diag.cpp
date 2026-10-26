// 诊断：J.ZAO 2560x1440@15 帧的实际 size 与理论值对比
#include <sparkcap.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    sparkcap_backend_t* backend = NULL;
    if (sparkcap_create_backend(&backend) != SPARKCAP_ERR_OK)
        return 1;
    int count = 0;
    sparkcap_list_devices(backend, NULL, &count);
    sparkcap_device_info_t devs[32];
    sparkcap_list_devices(backend, devs, &count);

    int id = -1;
    for (int i = 0; i < count; ++i)
        if (strstr(devs[i].name, "J.ZAO") != NULL)
            id = devs[i].id;
    if (id < 0) {
        printf("J.ZAO not found\n");
        return 1;
    }

    sparkcap_device_t* dev = NULL;
    sparkcap_open(backend, id, &dev);
    int rc = sparkcap_start(dev, 2560, 1440, SPARKCAP_PIXFMT_NV12, 15);
    printf("start(2560x1440 NV12@15) rc=%d\n", rc);
    if (rc != SPARKCAP_ERR_OK) {
        sparkcap_close(dev);
        sparkcap_destroy_backend(backend);
        return 1;
    }

    for (int i = 0; i < 5; ++i) {
        sparkcap_frame_t* f = NULL;
        rc = sparkcap_get_frame(dev, &f, 5000);
        if (rc == SPARKCAP_ERR_OK && f != NULL) {
            const long long theory = (long long)f->stride * f->height * 3 / 2;
            printf("frame %d: %dx%d fmt=%d stride=%d size=%d (theory stride*h*1.5=%lld "
                   "delta=%lld)\n",
                   i, f->width, f->height, (int)f->pixelFormat, f->stride, f->size, theory,
                   (long long)f->size - theory);
        } else {
            printf("frame %d: rc=%d\n", i, rc);
        }
    }
    sparkcap_stop(dev);
    sparkcap_close(dev);
    sparkcap_destroy_backend(backend);
    return 0;
}
