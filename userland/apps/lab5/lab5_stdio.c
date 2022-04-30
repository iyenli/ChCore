/*
 * Copyright (c) 2022 Institute of Parallel And Distributed Systems (IPADS)
 * ChCore-Lab is licensed under the Mulan PSL v1.
 * You can use this software according to the terms and conditions of the Mulan PSL v1.
 * You may obtain a copy of Mulan PSL v1 at:
 *     http://license.coscl.org.cn/MulanPSL
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY OR FIT FOR A PARTICULAR
 * PURPOSE.
 * See the Mulan PSL v1 for more details.
 */

#include "lab5_stdio.h"

extern struct ipc_struct* tmpfs_ipc_struct;

/* You could add new functions or include headers here.*/
/* LAB 5 TODO BEGIN */

#define MY_OFFSETOF(PodType, c) ((size_t) & (((PodType*)0)->c))

int alloc_fd()
{
    static int cnt = 0;
    return ++cnt;
}

#define MAX_PARAM_NUM 5
#define MAX_ONE_SEC_LEN 128
#define SUM_BUF 4096

/* A utility function to reverse a string  */
void reverse(char str[], int length)
{
    int start = 0;
    int end = length - 1;
    while (start < end) {
        char tmp = *(str + start);
        *(str + start) = *(str + end);
        *(str + end) = tmp;
        start++;
        end--;
    }
}

// Implementation of itoa()
char* itoa(int num, char* str, int base)
{
    int i = 0;
    bool isNegative = false;

    /* Handle 0 explicitly, otherwise empty string is printed for 0 */
    if (num == 0) {
        str[i++] = '0';
        str[i] = '\0';
        return str;
    }

    // In standard itoa(), negative numbers are handled only with
    // base 10. Otherwise numbers are considered unsigned.
    if (num < 0 && base == 10) {
        isNegative = true;
        num = -num;
    }

    // Process individual digits
    while (num != 0) {
        int rem = num % base;
        str[i++] = (rem > 9) ? (rem - 10) + 'a' : rem + '0';
        num = num / base;
    }

    // If number is negative, append '-'
    if (isNegative)
        str[i++] = '-';

    str[i] = '\0'; // Append string terminator

    // Reverse the string
    reverse(str, i);

    return str;
}

int atoi(char* str)
{
    // Initialize result
    int res = 0;

    for (int i = 0; str[i] != '\0'; ++i)
        res = res * 10 + str[i] - '0';

    return res;
}
/* LAB 5 TODO END */

FILE* fopen(const char* filename, const char* mode)
{

    /* LAB 5 TODO BEGIN */
    int err, i;
    struct ipc_msg* ipc_msg;
    int new_fd = alloc_fd();
    FILE* res = NULL;

open:
    i = FS_REQ_OPEN;
    ipc_msg = ipc_create_msg(tmpfs_ipc_struct, sizeof(struct fs_request), 0);
    ipc_set_msg_data(ipc_msg, (char*)&i, MY_OFFSETOF(struct fs_request, req), sizeof(i));
    ipc_set_msg_data(ipc_msg, filename, MY_OFFSETOF(struct fs_request, open.pathname), strlen(filename));
    ipc_set_msg_data(ipc_msg, (char*)&new_fd, MY_OFFSETOF(struct fs_request, open.new_fd), sizeof(new_fd));
    err = ipc_call(tmpfs_ipc_struct, ipc_msg);
    ipc_destroy_msg(tmpfs_ipc_struct, ipc_msg);

    if (err < 0) {
        i = FS_REQ_CREAT;
        ipc_msg = ipc_create_msg(tmpfs_ipc_struct, sizeof(struct fs_request), 0);
        ipc_set_msg_data(ipc_msg, (char*)&i, MY_OFFSETOF(struct fs_request, req), sizeof(i));
        ipc_set_msg_data(ipc_msg, filename, MY_OFFSETOF(struct fs_request, creat.pathname), strlen(filename));
        err = ipc_call(tmpfs_ipc_struct, ipc_msg);
        ipc_destroy_msg(tmpfs_ipc_struct, ipc_msg);
        goto open;
    }

    res = (struct FILE*)malloc(sizeof(struct FILE));
    BUG_ON(res == NULL);

    res->fd = new_fd;
    res->ptr = 0;
    for (i = 0; i < strlen(mode); i++) {
        if (mode[i] == 'r') {
            res->access[0] = 1;
        } else if (mode[i] == 'w') {
            res->access[1] = 1;
        } else if (mode[i] == 'x') {
            res->access[2] = 1;
        }
    }

    /* LAB 5 TODO END */
    return res;
}

size_t fwrite(const void* src, size_t size, size_t nmemb, FILE* f)
{
    /* LAB 5 TODO BEGIN */
    int err, i;
    struct ipc_msg* ipc_msg;
    BUG_ON(f == NULL);

    size_t write_count = size * nmemb;
    ipc_msg = ipc_create_msg(tmpfs_ipc_struct, sizeof(struct fs_request) + write_count, 0);

    i = FS_REQ_WRITE;
    ipc_set_msg_data(ipc_msg, (char*)&i, MY_OFFSETOF(struct fs_request, req), sizeof(i));
    ipc_set_msg_data(ipc_msg, (char*)&(f->fd), MY_OFFSETOF(struct fs_request, write.fd), sizeof(f->fd));
    ipc_set_msg_data(ipc_msg, (char*)&write_count, MY_OFFSETOF(struct fs_request, write.count), sizeof(write_count));
    memcpy(ipc_get_msg_data(ipc_msg) + sizeof(struct fs_request), src, write_count);
    err = ipc_call(tmpfs_ipc_struct, ipc_msg);
    ipc_destroy_msg(tmpfs_ipc_struct, ipc_msg);

    /* LAB 5 TODO END */
    return err;
}

size_t fread(void* destv, size_t size, size_t nmemb, FILE* f)
{
    /* LAB 5 TODO BEGIN */
    int err, i;
    struct ipc_msg* ipc_msg;
    BUG_ON(f == NULL);

    size_t read_count = size * nmemb;
    ipc_msg = ipc_create_msg(tmpfs_ipc_struct, sizeof(struct fs_request) + read_count, 0);

    i = FS_REQ_READ;
    ipc_set_msg_data(ipc_msg, (char*)&i, MY_OFFSETOF(struct fs_request, req), sizeof(i));
    ipc_set_msg_data(ipc_msg, (char*)&(f->fd), MY_OFFSETOF(struct fs_request, read.fd), sizeof(f->fd));
    ipc_set_msg_data(ipc_msg, (char*)&read_count, MY_OFFSETOF(struct fs_request, read.count), sizeof(read_count));
    err = ipc_call(tmpfs_ipc_struct, ipc_msg);
    ipc_destroy_msg(tmpfs_ipc_struct, ipc_msg);

    if (err <= 0) {
        return 0;
    }
    memcpy(destv, ipc_get_msg_data(ipc_msg), err);

    /* LAB 5 TODO END */
    return err;
}

int fclose(FILE* f)
{
    /* LAB 5 TODO BEGIN */
    int err, i;
    struct ipc_msg* ipc_msg;
    BUG_ON(f == NULL);

    ipc_msg = ipc_create_msg(tmpfs_ipc_struct, sizeof(struct fs_request), 0);

    i = FS_REQ_CLOSE;
    ipc_set_msg_data(ipc_msg, (char*)&i, MY_OFFSETOF(struct fs_request, req), sizeof(i));
    ipc_set_msg_data(ipc_msg, (char*)&(f->fd), MY_OFFSETOF(struct fs_request, close.fd), sizeof(f->fd));
    err = ipc_call(tmpfs_ipc_struct, ipc_msg);
    ipc_destroy_msg(tmpfs_ipc_struct, ipc_msg);
    /* LAB 5 TODO END */
    return err;
}

/* Need to support %s and %d. */
int fscanf(FILE* f, const char* fmt, ...)
{
    /* LAB 5 TODO BEGIN */
    int err, flag, param = 0;
    size_t i = 0, j = 0, len = strlen(fmt);

    /* handle fxxking string */
    char type[MAX_PARAM_NUM];
    char buf[MAX_PARAM_NUM + 1][MAX_ONE_SEC_LEN];
    char summary_buf[SUM_BUF];

    err = fread(summary_buf, 1024, 1, f);
    BUG_ON(err != strlen(summary_buf));

    for (; i < len; ++i) {
        BUG_ON(j == err);

        if (fmt[i] == '%') {
            BUG_ON(i == len - 1);
            if (fmt[++i] == 's') {
                type[param] = 1;
                if (i == len - 1) {
                    strcat(buf[param++], summary_buf + j);
                } else {
                    flag = 0; // store string in buf[param] until match
                    while (j < err && summary_buf[j] != fmt[i + 1]) {
                        buf[param][flag++] = summary_buf[j++];
                    }

                    buf[param++][flag] = '\0';
                }

            } else if (fmt[i] == 'd') {
                type[param] = 2;
                flag = 0; // store string in buf[param] until match
                while (j < err && summary_buf[j] >= '0' && summary_buf[j] <= '9') {
                    buf[param][flag++] = summary_buf[j++];
                }
                buf[param++][flag] = '\0';
            } else {
                BUG("Not supported type\n");
            }
            BUG_ON(param >= MAX_PARAM_NUM);
        } else {
            if (summary_buf[j] != fmt[i]) {
                WARN("Not matched in fscanf.\n");
                return -1;
            }
            ++j; // i will be added later
        }
    }

    va_list valist;
    va_start(valist, param);

    for (i = 0; i < param; i++) {
        if (type[i] == 1) {
            strcat(va_arg(valist, char*), buf[i]);
        } else {
            *va_arg(valist, int*) = atoi(buf[i]);
        }
    }
    va_end(valist);
    /* END */

    /* LAB 5 TODO END */
    return 0;
}

/* Need to support %s and %d. */
int fprintf(FILE* f, const char* fmt, ...)
{
    /* LAB 5 TODO BEGIN */
    int err, flag = 0, param = 0;
    size_t i = 0, len = strlen(fmt);

    /* handle fxxking string */
    char type[MAX_PARAM_NUM];
    char buf[MAX_PARAM_NUM + 1][MAX_ONE_SEC_LEN];
    char summary_buf[SUM_BUF];

    for (; i < len; ++i) {
        if (fmt[i] == '%') {
            BUG_ON(i == len - 1);

            ++i;
            type[param] = (fmt[i] == 'd') ? 1 : (fmt[i] == 's') ? 2
                                                                : 0;
            BUG_ON(type[param] == 0);

            buf[param++][flag] = '\0';
            flag = 0;

            BUG_ON(param >= MAX_PARAM_NUM);
        } else {
            buf[param][flag++] = fmt[i];
        }
    }
    buf[param][flag] = '\0';

    va_list valist;
    va_start(valist, param);
    summary_buf[0] = '\0';
    for (i = 0; i < param; i++) {
        strcat(summary_buf, buf[i]);
        if (type[i] == 1) {
            strcat(summary_buf, itoa(va_arg(valist, int), buf[i], 10));
        } else {
            strcat(summary_buf, va_arg(valist, char*));
        }
    }
    strcat(summary_buf, buf[param]);
    va_end(valist);
    /* END */
    err = fwrite(summary_buf, strlen(summary_buf), 1, f);
    /* LAB 5 TODO END */
    return err;
}
