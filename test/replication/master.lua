#!/usr/bin/env tarantool_box
os = require('os')
box.cfg({
    primary_port        = os.getenv("PRIMARY_PORT"),
    admin_port          = os.getenv("ADMIN_PORT"),
    slab_alloc_arena    = 0.1,
    pid_file            = "tarantool.pid",
    logger              = "cat - >> tarantool.log",
    custom_proc_title   = "master",
})