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
#include <chcore/fsm.h>
#include <chcore/internal/server_caps.h>
#include <chcore/ipc.h>
#include <string.h>

static struct ipc_struct* fsm_ipc_struct = NULL;
static struct list_head fs_cap_infos;

/* Helper function */
#define MY_OFFSETOF(PodType, c) ((size_t) & (((PodType*)0)->c))

int alloc_new_fd()
{
    static int cnt = 0;
    return ++cnt;
}
/* Helper function */

struct fs_cap_info_node {
    int fs_cap;
    ipc_struct_t* fs_ipc_struct;
    struct list_head node;
};

struct fs_cap_info_node* set_fs_cap_info(int fs_cap)
{
    struct fs_cap_info_node* n;
    n = (struct fs_cap_info_node*)malloc(sizeof(*n));
    chcore_assert(n);
    n->fs_ipc_struct = ipc_register_client(fs_cap);
    chcore_assert(n->fs_ipc_struct);
    list_add(&n->node, &fs_cap_infos);
    return n;
}

/* Search for the fs whose capability is `fs_cap`.*/
struct fs_cap_info_node* get_fs_cap_info(int fs_cap)
{
    struct fs_cap_info_node* iter;
    struct fs_cap_info_node* matched_fs = NULL;
    for_each_in_list(iter, struct fs_cap_info_node, node, &fs_cap_infos)
    {
        if (iter->fs_cap == fs_cap) {
            matched_fs = iter;
            break;
        }
    }
    if (!matched_fs) {
        return set_fs_cap_info(fs_cap);
    }
    return matched_fs;
}

static void connect_fsm_server(void)
{
    init_list_head(&fs_cap_infos);
    int fsm_cap = __chcore_get_fsm_cap();
    chcore_assert(fsm_cap >= 0);
    fsm_ipc_struct = ipc_register_client(fsm_cap);
    chcore_assert(fsm_ipc_struct);
}

int fsm_creat_file(char* path)
{
    if (!fsm_ipc_struct) {
        connect_fsm_server();
    }
    struct ipc_msg* ipc_msg = ipc_create_msg(
        fsm_ipc_struct, sizeof(struct fs_request), 0);
    chcore_assert(ipc_msg);
    struct fs_request* fr = (struct fs_request*)ipc_get_msg_data(ipc_msg);
    fr->req = FS_REQ_CREAT;
    strcpy(fr->creat.pathname, path);
    int ret = ipc_call(fsm_ipc_struct, ipc_msg);
    ipc_destroy_msg(fsm_ipc_struct, ipc_msg);
    return ret;
}

int get_file_size_from_fsm(char* path)
{
    if (!fsm_ipc_struct) {
        connect_fsm_server();
    }
    struct ipc_msg* ipc_msg = ipc_create_msg(
        fsm_ipc_struct, sizeof(struct fs_request), 0);
    chcore_assert(ipc_msg);
    struct fs_request* fr = (struct fs_request*)ipc_get_msg_data(ipc_msg);

    fr->req = FS_REQ_GET_SIZE;
    strcpy(fr->getsize.pathname, path);

    int ret = ipc_call(fsm_ipc_struct, ipc_msg);
    ipc_destroy_msg(fsm_ipc_struct, ipc_msg);
    return ret;
}

struct fs_cap_info_node* get_fs(const char* path, char** leaf)
{
    int i;
    struct ipc_msg* ipc_msg;

    // const char* can't serve as param
    char* tmp = malloc(strlen(path) + 1);
    tmp[0] = '\0';
    strcat(tmp, path);

    ipc_msg = ipc_create_msg(fsm_ipc_struct, sizeof(struct fs_request), 0);
    i = FS_REQ_GET_FS_CAP;
    ipc_set_msg_data(ipc_msg, (char*)&i, MY_OFFSETOF(struct fs_request, req), sizeof(i));
    ipc_set_msg_data(ipc_msg, tmp, MY_OFFSETOF(struct fs_request, getfscap.pathname), strlen(tmp));

    ipc_call(fsm_ipc_struct, ipc_msg);
    u64 cap = ipc_get_msg_cap(ipc_msg, 0);
    *leaf = tmp;

    ipc_destroy_msg(fsm_ipc_struct, ipc_msg);
    return get_fs_cap_info(cap);
}

// Just copy... so tired now
int open_file(char* filename, struct fs_cap_info_node* fs)
{
    int err, i;
    struct ipc_msg* ipc_msg;
    int new_fd = alloc_new_fd();

open:
    i = FS_REQ_OPEN;
    ipc_msg = ipc_create_msg(fs->fs_ipc_struct, sizeof(struct fs_request), 0);
    ipc_set_msg_data(ipc_msg, (char*)&i, MY_OFFSETOF(struct fs_request, req), sizeof(i));
    ipc_set_msg_data(ipc_msg, filename, MY_OFFSETOF(struct fs_request, open.pathname), strlen(filename));
    ipc_set_msg_data(ipc_msg, (char*)&new_fd, MY_OFFSETOF(struct fs_request, open.new_fd), sizeof(new_fd));
    err = ipc_call(fs->fs_ipc_struct, ipc_msg);
    ipc_destroy_msg(fs->fs_ipc_struct, ipc_msg);

    if (err < 0) {
        i = FS_REQ_CREAT;
        ipc_msg = ipc_create_msg(fs->fs_ipc_struct, sizeof(struct fs_request), 0);
        ipc_set_msg_data(ipc_msg, (char*)&i, MY_OFFSETOF(struct fs_request, req), sizeof(i));
        ipc_set_msg_data(ipc_msg, filename, MY_OFFSETOF(struct fs_request, creat.pathname), strlen(filename));
        err = ipc_call(fs->fs_ipc_struct, ipc_msg);
        ipc_destroy_msg(fs->fs_ipc_struct, ipc_msg);
        goto open;
    }

    return err;
}

/* Write buf into the file at `path`. */
int fsm_write_file(const char* path, char* buf, unsigned long size)
{
    if (!fsm_ipc_struct) {
        connect_fsm_server();
    }
    int ret = 0, i = 0;
    char* leaf;

    /* LAB 5 TODO BEGIN */
    // Copy...
    struct ipc_msg* ipc_msg;
    struct fs_cap_info_node* fs = get_fs(path, &leaf);
    int fd = open_file(leaf, fs);

    ipc_msg = ipc_create_msg(fs->fs_ipc_struct, size + sizeof(struct fs_request), 0);

    i = FS_REQ_WRITE;
    ipc_set_msg_data(ipc_msg, (char*)&i, MY_OFFSETOF(struct fs_request, req), sizeof(i));
    ipc_set_msg_data(ipc_msg, (char*)&(fd), MY_OFFSETOF(struct fs_request, write.fd), sizeof(fd));
    ipc_set_msg_data(ipc_msg, (char*)&size, MY_OFFSETOF(struct fs_request, write.count), sizeof(size));
    memcpy(ipc_get_msg_data(ipc_msg) + sizeof(struct fs_request), buf, size);
    ret = ipc_call(fs->fs_ipc_struct, ipc_msg);
    ipc_destroy_msg(fs->fs_ipc_struct, ipc_msg);
    /* LAB 5 TODO END */

    return ret;
}

/* Read content from the file at `path`. */
int fsm_read_file(const char* path, char* buf, unsigned long size)
{
    if (!fsm_ipc_struct) {
        connect_fsm_server();
    }
    int ret = 0, i = 0;
    char* leaf;

    /* LAB 5 TODO BEGIN */
    struct ipc_msg* ipc_msg;
    struct fs_cap_info_node* fs = get_fs(path, &leaf);
    int fd = open_file(leaf, fs);

    ipc_msg = ipc_create_msg(fs->fs_ipc_struct, size + sizeof(struct fs_request), 0);

    i = FS_REQ_READ;
    ipc_set_msg_data(ipc_msg, (char*)&i, MY_OFFSETOF(struct fs_request, req), sizeof(i));
    ipc_set_msg_data(ipc_msg, (char*)&(fd), MY_OFFSETOF(struct fs_request, read.fd), sizeof(fd));
    ipc_set_msg_data(ipc_msg, (char*)&size, MY_OFFSETOF(struct fs_request, read.count), sizeof(size));
    ret = ipc_call(fs->fs_ipc_struct, ipc_msg);
    ipc_destroy_msg(fs->fs_ipc_struct, ipc_msg);

    if (ret <= 0) {
        return 0;
    }
    memcpy(buf, ipc_get_msg_data(ipc_msg), ret);
    /* LAB 5 TODO END */

    return ret;
}

void chcore_fsm_test()
{
    if (!fsm_ipc_struct) {
        connect_fsm_server();
    }
    char wbuf[257];
    char rbuf[257];
    memset(rbuf, 0, sizeof(rbuf));
    memset(wbuf, 'x', sizeof(wbuf));
    wbuf[256] = '\0';
    fsm_creat_file("/fakefs/fsmtest.txt");
    fsm_write_file("/fakefs/fsmtest.txt", wbuf, sizeof(wbuf));
    fsm_read_file("/fakefs/fsmtest.txt", rbuf, sizeof(rbuf));
    int res = memcmp(wbuf, rbuf, strlen(wbuf));
    if (res == 0) {
        printf("chcore fsm bypass test pass\n");
    }
}
