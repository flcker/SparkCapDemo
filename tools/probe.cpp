// Copyright (c) 2026 flcker <molanyousi@gmail.com>
// SPDX-License-Identifier: LGPL-3.0-or-later

// 探针：逐台设备 open → start（无偏好）→ 取帧 → 打印实际 pixel_format/stride/size
#include <sparkcap.h>
#include <stdio.h>

int main(void)
{
    sparkcap_backend_t* backend = NULL;
    if (sparkcap_create_backend(&backend) != SPARKCAP_ERR_OK)
        return 1;

    int count = 0;
    sparkcap_camera_list_devices(backend, NULL, &count);
    sparkcap_device_info_t devs[32];
    sparkcap_camera_list_devices(backend, devs, &count);

    for (int i = 0; i < count; ++i) {
        printf("[%d] %s (backend %d)\n", devs[i].id, devs[i].name, (int)devs[i].backend);

        sparkcap_device_t* dev = NULL;
        int rc = sparkcap_camera_open(backend, devs[i].id, &dev);
        printf("  open rc=%d\n", rc);
        if (rc != SPARKCAP_ERR_OK)
            continue;

        rc = sparkcap_camera_start(dev, 0, 0, SPARKCAP_PIXFMT_UNKNOWN, 0);
        printf("  start(default) rc=%d\n", rc);
        if (rc != SPARKCAP_ERR_OK) {
            sparkcap_camera_close(dev);
            continue;
        }

        sparkcap_frame_t* f = NULL;
        rc = sparkcap_camera_get_frame(dev, &f, 5000);
        if (rc == SPARKCAP_ERR_OK)
            printf("  frame: %dx%d fmt=%d stride=%d size=%d ts=%lld\n", f->width,
                   f->height, (int)f->pixel_format, f->stride, f->size,
                   (long long)f->timestamp);
        else
            printf("  get_frame rc=%d\n", rc);
        sparkcap_camera_stop(dev);
        sparkcap_camera_close(dev);

        // 对取不到帧的设备：逐个能力条目重试（前 12 条）
        if (rc != SPARKCAP_ERR_OK && f == NULL) {
            printf("  retrying with each format...\n");
            int n = 64;
            sparkcap_device_format_t fmts[64];
            if (sparkcap_camera_get_formats(backend, devs[i].id, fmts, &n) == SPARKCAP_ERR_OK) {
                if (n > 64)
                    n = 64;  // get_formats 更新为总数，可能 > 容量
                const int tryMax = n < 12 ? n : 12;
                for (int k = 0; k < tryMax; ++k) {
                    sparkcap_device_t* d2 = NULL;
                    if (sparkcap_camera_open(backend, devs[i].id, &d2) != SPARKCAP_ERR_OK)
                        continue;
                    const int r2 = sparkcap_camera_start(d2, fmts[k].width, fmts[k].height,
                                                  fmts[k].pixel_format, fmts[k].fps);
                    if (r2 == SPARKCAP_ERR_OK) {
                        sparkcap_frame_t* f2 = NULL;
                        const int r3 = sparkcap_camera_get_frame(d2, &f2, 3000);
                        printf("    %dx%d fmt=%d fps=%d -> start=%d frame=%d", fmts[k].width,
                               fmts[k].height, (int)fmts[k].pixel_format, fmts[k].fps, r2,
                               r3);
                        if (r3 == SPARKCAP_ERR_OK && f2 != NULL)
                            printf(" (actual %dx%d fmt=%d stride=%d)", f2->width, f2->height,
                                   (int)f2->pixel_format, f2->stride);
                        printf("\n");
                        sparkcap_camera_stop(d2);
                    } else {
                        printf("    %dx%d fmt=%d fps=%d -> start rc=%d\n", fmts[k].width,
                               fmts[k].height, (int)fmts[k].pixel_format, fmts[k].fps, r2);
                    }
                    sparkcap_camera_close(d2);
                }
                // 按唯一分辨率全扫描：每个 (w,h) 挑一个非 MJPG 条目实测出帧
                printf("  full resolution scan:\n");
                for (int i2 = 0; i2 < n; ++i2) {
                    if (fmts[i2].pixel_format == SPARKCAP_PIXFMT_MJPG)
                        continue;
                    // 同分辨率的非 MJPG 只试一条（fps 最低的通常第一个遇到的先试）
                    bool dup = false;
                    for (int j = 0; j < i2; ++j)
                        if (fmts[j].width == fmts[i2].width &&
                            fmts[j].height == fmts[i2].height &&
                            fmts[j].pixel_format != SPARKCAP_PIXFMT_MJPG)
                            dup = true;
                    if (dup)
                        continue;

                    sparkcap_device_t* d2 = NULL;
                    if (sparkcap_camera_open(backend, devs[i].id, &d2) != SPARKCAP_ERR_OK)
                        continue;
                    const int r2 = sparkcap_camera_start(d2, fmts[i2].width, fmts[i2].height,
                                                  fmts[i2].pixel_format, fmts[i2].fps);
                    if (r2 == SPARKCAP_ERR_OK) {
                        sparkcap_frame_t* f2 = NULL;
                        const int r3 = sparkcap_camera_get_frame(d2, &f2, 4000);
                        printf("    %dx%d fmt=%d fps=%d -> frame rc=%d", fmts[i2].width,
                               fmts[i2].height, (int)fmts[i2].pixel_format, fmts[i2].fps,
                               r3);
                        if (r3 == SPARKCAP_ERR_OK && f2 != NULL)
                            printf("  *** WORKS *** (actual %dx%d fmt=%d stride=%d)",
                                   f2->width, f2->height, (int)f2->pixel_format,
                                   f2->stride);
                        printf("\n");
                        sparkcap_camera_stop(d2);
                    } else {
                        printf("    %dx%d fmt=%d fps=%d -> start rc=%d\n", fmts[i2].width,
                               fmts[i2].height, (int)fmts[i2].pixel_format, fmts[i2].fps,
                               r2);
                    }
                    sparkcap_camera_close(d2);
                }
            }
        }
    }
    sparkcap_destroy_backend(backend);
    return 0;
}
