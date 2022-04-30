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

#include <chcore/assert.h>
#include <chcore/fs/defs.h>
#include <chcore/internal/raw_syscall.h>
#include <chcore/internal/server_caps.h>
#include <chcore/ipc.h>
#include <chcore/procm.h>
#include <chcore/thread.h>
#include <malloc.h>
#include <stdio.h>
#include <string.h>

#define MY_OFFSETOF(PodType, c) ((size_t) & (((PodType*)0)->c))
#define SERVER_READY_FLAG(vaddr) (*(int*)(vaddr))
#define SERVER_EXIT_FLAG(vaddr) (*(int*)((u64)vaddr + 4))

static struct ipc_struct ipc_struct;
static int tmpfs_scan_pmo_cap;

/* fs_server_cap in current process; can be copied to others */
int fs_server_cap;

#define BUFLEN 4096

struct ipc_struct* fs_ipc_struct_for_shell = NULL;

static void connect_tmpfs_server(void)
{

    int tmpfs_cap = __chcore_get_tmpfs_cap();
    chcore_assert(tmpfs_cap >= 0);
    fs_ipc_struct_for_shell = ipc_register_client(tmpfs_cap);
    chcore_assert(fs_ipc_struct_for_shell);
}

int mount_fs(const char* fspath, const char* mount_point)
{
    int ret;
    struct ipc_msg* ipc_msg = ipc_create_msg(
        fs_ipc_struct_for_shell, sizeof(struct fs_request), 0);
    chcore_assert(ipc_msg);
    struct fs_request* fr = (struct fs_request*)ipc_get_msg_data(ipc_msg);
    fr->req = FS_REQ_MOUNT;
    strcpy(fr->mount.fs_path, fspath);
    strcpy(fr->mount.mount_path, mount_point);
    ret = ipc_call(fs_ipc_struct_for_shell, ipc_msg);
    ipc_destroy_msg(fs_ipc_struct_for_shell, ipc_msg);
    return ret;
}

/* Retrieve the entry name from one dirent */
static void get_dent_name(struct dirent* p, char name[])
{
    int len;
    len = p->d_reclen - sizeof(p->d_ino) - sizeof(p->d_off)
        - sizeof(p->d_reclen) - sizeof(p->d_type);
    memcpy(name, p->d_name, len);
    name[len - 1] = '\0';
}

/* See demo_getdents() for example */
int getdents(int fd, char* buf, int count)
{
    struct ipc_msg* ipc_msg = 0;
    struct fd_record_extension* fd_ext;
    struct fs_request* fr_ptr;
    int ret = 0, remain = count, cnt;
    chcore_assert(fs_ipc_struct_for_shell);

    ipc_msg = ipc_create_msg(fs_ipc_struct_for_shell, 512, 0);
    fr_ptr = (struct fs_request*)ipc_get_msg_data(ipc_msg);
    while (remain > 0) {
        fr_ptr->req = FS_REQ_GETDENTS64;
        fr_ptr->getdents64.fd = fd;
        cnt = MIN(remain, PAGE_SIZE);
        fr_ptr->getdents64.count = cnt;
        ret = ipc_call(fs_ipc_struct_for_shell, ipc_msg);
        if (ret < 0)
            goto error;
        memcpy(buf, ipc_get_msg_data(ipc_msg), ret);
        buf += ret;
        remain -= ret;
        if (ret != cnt)
            break;
    }
    ret = count - remain;
error:
    ipc_destroy_msg(fs_ipc_struct_for_shell, ipc_msg);
    return ret;
}

void demo_getdents(int fd)
{
    char name[BUFLEN];
    char scan_buf[BUFLEN];
    int offset;
    struct dirent* p;

    int ret = getdents(fd, scan_buf, BUFLEN);

    for (offset = 0; offset < ret; offset += p->d_reclen) {
        p = (struct dirent*)(scan_buf + offset);
        get_dent_name(p, name);
        printf("The name is %s \n", name);
    }
}

int alloc_fd()
{
    static int cnt = 0;
    return ++cnt;
}

int do_complement(char* buf, char* complement, int complement_time)
{
    int ret = 0, j = 0;
    struct dirent* p;
    char name[BUFLEN];
    char scan_buf[BUFLEN];
    int r = -1;
    int offset;
    char path[2] = "/\0"; // only support root dir:)
    scan_buf[0] = '\0';

    /* LAB 5 TODO BEGIN */
    // Get all possible names
    int new_fd = alloc_fd();
    struct ipc_msg* ipc_msg;

    ipc_msg = ipc_create_msg(fs_ipc_struct_for_shell, sizeof(struct fs_request), 0);
    j = FS_REQ_OPEN;
    ipc_set_msg_data(ipc_msg, (char*)&j, MY_OFFSETOF(struct fs_request, req), sizeof(j));
    ipc_set_msg_data(ipc_msg, path, MY_OFFSETOF(struct fs_request, open.pathname), strlen(path));
    ipc_set_msg_data(ipc_msg, (char*)&new_fd, MY_OFFSETOF(struct fs_request, open.new_fd), sizeof(new_fd));
    // Ignore mod and flag
    ret = ipc_call(fs_ipc_struct_for_shell, ipc_msg);
    ipc_destroy_msg(fs_ipc_struct_for_shell, ipc_msg);

    ipc_msg = ipc_create_msg(fs_ipc_struct_for_shell, sizeof(struct fs_request), 0);
    j = FS_REQ_GETDENTS64;
    ipc_set_msg_data(ipc_msg, (char*)&j, MY_OFFSETOF(struct fs_request, req), sizeof(j));
    ipc_set_msg_data(ipc_msg, (char*)&new_fd, MY_OFFSETOF(struct fs_request, getdents64.fd), sizeof(new_fd));
    j = ipc_msg->data_len;
    ipc_set_msg_data(ipc_msg, (char*)&j, MY_OFFSETOF(struct fs_request, getdents64.count), sizeof(j));
    ret = ipc_call(fs_ipc_struct_for_shell, ipc_msg);

    offset = 0, j = 0;
    do {
        while (offset < ret) {
            p = ipc_get_msg_data(ipc_msg) + offset;
            get_dent_name(p, scan_buf);
            offset += p->d_reclen;

            if (scan_buf[0] != '.') { // hidden file
                if (complement_time == 0) {
                    complement[0] = '\0';
                    strcat(complement, scan_buf);
                    break;
                }
                --complement_time;
            }
            ++j;
        }
        if (complement_time == 0) {
            break;
        }
        complement_time %= j;
        offset = 0;
    } while (1);

    ipc_destroy_msg(fs_ipc_struct_for_shell, ipc_msg);
    /* LAB 5 TODO END */

    return 0;
}

extern char getch();

// read a command from stdin leading by `prompt`
// put the commond in `buf` and return `buf`
// What you typed should be displayed on the screen
char* readline(const char* prompt)
{
    static char buf[BUFLEN];

    int i = 0, j = 0;
    signed char c = 0;
    int ret = 0;
    char complement[BUFLEN];
    int complement_time = 0;

    if (prompt != NULL) {
        printf("%s", prompt);
    }

    while (1) {
        __chcore_sys_yield();
        c = getch();

        /* LAB 5 TODO BEGIN */
        /* Fill buf and handle tabs with do_complement(). */
        if (c == '\n') {
            printf("%c", c);

            complement_time = 0;
            buf[i] = '\0';

            break;
        } else if (c == '\t') {
            ret = do_complement(buf, complement, complement_time++);

            buf[0] = '\0';
            strcat(buf, complement);
            printf("%s", buf);
            i = strlen(buf);
        } else {
            buf[i++] = c;
            complement_time = 0;
            printf("%c", c);
        }
        /* LAB 5 TODO END */
    }

    return buf;
}

int do_top()
{
    __chcore_sys_top();
    return 0;
}

void print_file_content(char* path)
{
    /* LAB 5 TODO BEGIN */
    int new_fd = alloc_fd();
    int i, ret = 0;
    struct ipc_msg* ipc_msg;

    ipc_msg = ipc_create_msg(fs_ipc_struct_for_shell, sizeof(struct fs_request), 0);
    i = FS_REQ_OPEN;
    ipc_set_msg_data(ipc_msg, (char*)&i, MY_OFFSETOF(struct fs_request, req), sizeof(i));
    ipc_set_msg_data(ipc_msg, path, MY_OFFSETOF(struct fs_request, open.pathname), strlen(path));
    ipc_set_msg_data(ipc_msg, (char*)&new_fd, MY_OFFSETOF(struct fs_request, open.new_fd), sizeof(new_fd));
    // Ignore mod and flag
    ret = ipc_call(fs_ipc_struct_for_shell, ipc_msg);
    BUG_ON(ret < 0);

    ipc_destroy_msg(fs_ipc_struct_for_shell, ipc_msg);
    ipc_msg = ipc_create_msg(fs_ipc_struct_for_shell, sizeof(struct fs_request), 0);

    i = FS_REQ_READ;
    ipc_set_msg_data(ipc_msg, (char*)&i, MY_OFFSETOF(struct fs_request, req), sizeof(i));
    ipc_set_msg_data(ipc_msg, (char*)&new_fd, MY_OFFSETOF(struct fs_request, read.fd), sizeof(new_fd));
    i = ipc_msg->data_len;
    ipc_set_msg_data(ipc_msg, (char*)&i, MY_OFFSETOF(struct fs_request, read.count), sizeof(i));

    ret = ipc_call(fs_ipc_struct_for_shell, ipc_msg);
    printf("%s", ipc_get_msg_data(ipc_msg));
    ipc_destroy_msg(fs_ipc_struct_for_shell, ipc_msg);
    /* LAB 5 TODO END */
}

void fs_scan(char* path)
{

    /* LAB 5 TODO BEGIN */
    int new_fd = alloc_fd();
    int i, ret = 0;
    struct ipc_msg* ipc_msg;
    // TODO: Confirm the buffer size if bugs show.

    char buf[(MAX_FILENAME_LEN + 1) * 10], tmp_buf[MAX_FILENAME_LEN + 2];
    buf[0] = '\0', tmp_buf[0] = '\0';
    struct dirent* ptr;

    ipc_msg = ipc_create_msg(fs_ipc_struct_for_shell, sizeof(struct fs_request), 0);
    i = FS_REQ_OPEN;
    ipc_set_msg_data(ipc_msg, (char*)&i, MY_OFFSETOF(struct fs_request, req), sizeof(i));
    ipc_set_msg_data(ipc_msg, path, MY_OFFSETOF(struct fs_request, open.pathname), strlen(path));
    ipc_set_msg_data(ipc_msg, (char*)&new_fd, MY_OFFSETOF(struct fs_request, open.new_fd), sizeof(new_fd));
    // Ignore mod and flag
    ret = ipc_call(fs_ipc_struct_for_shell, ipc_msg);
    BUG_ON(ret < 0);

    ipc_destroy_msg(fs_ipc_struct_for_shell, ipc_msg);
    ipc_msg = ipc_create_msg(fs_ipc_struct_for_shell, sizeof(struct fs_request), 0);

    i = FS_REQ_GETDENTS64;
    ipc_set_msg_data(ipc_msg, (char*)&i, MY_OFFSETOF(struct fs_request, req), sizeof(i));
    ipc_set_msg_data(ipc_msg, (char*)&new_fd, MY_OFFSETOF(struct fs_request, getdents64.fd), sizeof(new_fd));
    i = ipc_msg->data_len;
    ipc_set_msg_data(ipc_msg, (char*)&i, MY_OFFSETOF(struct fs_request, getdents64.count), sizeof(i));

    ret = ipc_call(fs_ipc_struct_for_shell, ipc_msg);
    i = 0;

    while (i < ret) {
        ptr = ipc_get_msg_data(ipc_msg) + i;
        get_dent_name(ptr, tmp_buf);
        i += ptr->d_reclen;

        if (tmp_buf[0] != '.') { // hidden file
            if (i < ret) { // BUGGY: what if the last dentry is not complete?
                size_t len = strlen(tmp_buf);
                tmp_buf[len] = ' ';
                tmp_buf[len + 1] = '\0';
            }
            strcat(buf, tmp_buf);
        }

        tmp_buf[0] = '\0';
    }

    printf("%s", buf);
    ipc_destroy_msg(fs_ipc_struct_for_shell, ipc_msg);
    /* LAB 5 TODO END */
}

int do_ls(char* cmdline)
{
    char pathbuf[BUFLEN];

    pathbuf[0] = '\0';
    cmdline += 2;
    while (*cmdline == ' ')
        cmdline++;
    strcat(pathbuf, cmdline);
    fs_scan(pathbuf); /* LAB 5: Complete fs_scan() */
    return 0;
}

int do_cat(char* cmdline)
{
    char pathbuf[BUFLEN];

    pathbuf[0] = '\0';
    cmdline += 3;
    while (*cmdline == ' ')
        cmdline++;
    strcat(pathbuf, cmdline);
    print_file_content(pathbuf); /* LAB 5: Complete print_file_content() */
    return 0;
}

int do_echo(char* cmdline)
{
    /* LAB 5 TODO BEGIN */
    char pathbuf[BUFLEN];
    cmdline += 4;
    while (*cmdline == ' ')
        cmdline++;

    pathbuf[0] = '\0';
    strcat(pathbuf, cmdline);

    printf("%s", pathbuf);
    /* LAB 5 TODO END */
    return 0;
}

void do_clear(void)
{
    putc(12);
    putc(27);
    putc('[');
    putc('2');
    putc('J');
}

int builtin_cmd(char* cmdline)
{
    int ret, i;
    char cmd[BUFLEN];
    for (i = 0; cmdline[i] != ' ' && cmdline[i] != '\0'; i++)
        cmd[i] = cmdline[i];
    cmd[i] = '\0';
    if (!strcmp(cmd, "quit") || !strcmp(cmd, "exit"))
        __chcore_sys_thread_exit();
    if (!strcmp(cmd, "ls")) {
        ret = do_ls(cmdline);
        return !ret ? 1 : -1;
    }
    if (!strcmp(cmd, "echo")) {
        ret = do_echo(cmdline);
        return !ret ? 1 : -1;
    }
    if (!strcmp(cmd, "cat")) {
        ret = do_cat(cmdline);
        return !ret ? 1 : -1;
    }
    if (!strcmp(cmd, "clear")) {
        do_clear();
        return 1;
    }
    if (!strcmp(cmd, "top")) {
        ret = do_top();
        return !ret ? 1 : -1;
    }
    return 0;
}

int run_cmd(char* cmdline)
{
    int cap = 0;
    /* Hint: Function chcore_procm_spawn() could be used here. */
    /* LAB 5 TODO BEGIN */
    chcore_procm_spawn(cmdline, cap);
    /* LAB 5 TODO END */
    return 0;
}

void connect_fs(void)
{
    // register fs client
    connect_tmpfs_server();
}
