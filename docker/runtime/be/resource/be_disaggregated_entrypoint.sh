#!/bin/bash
# Licensed to the Apache Software Foundation (ASF) under one
# or more contributor license agreements.  See the NOTICE file
# distributed with this work for additional information
# regarding copyright ownership.  The ASF licenses this file
# to you under the Apache License, Version 2.0 (the
# "License"); you may not use this file except in compliance
# with the License.  You may obtain a copy of the License at
#
#   http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing,
# software distributed under the License is distributed on an
# "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
# KIND, either express or implied.  See the License for the
# specific language governing permissions and limitations
# under the License.


# the fe query port for mysql.
FE_QUERY_PORT=${FE_QUERY_PORT:-9030}
# timeout for probe fe master.
PROBE_TIMEOUT=60
# interval time to probe fe.
PROBE_INTERVAL=2
# rpc port for fe communicate with be.
HEARTBEAT_PORT=9050
# fqdn or ip
MY_SELF=
MY_IP=`hostname -i`
MY_HOSTNAME=`hostname -f`
DORIS_ROOT=${DORIS_ROOT:-"/opt/apache-doris"}
# if config secret for basic auth about operate node of doris, the path must be `/etc/basic_auth`. This is set by operator and the key of password must be `password`.
AUTH_PATH="/etc/basic_auth"
DORIS_HOME=${DORIS_ROOT}/be
BE_CONFIG=$DORIS_HOME/conf/be.conf
# represents self in fe meta or not.
REGISTERED=false

DB_ADMIN_USER=${USER:-"root"}

DB_ADMIN_PASSWD=$PASSWD

COMPUTE_GROUP_NAME=${COMPUTE_GROUP_NAME}

ENABLE_WORKLOAD_GROUP=${ENABLE_WORKLOAD_GROUP:-false}
WORKLOAD_GROUP_PATH="/sys/fs/cgroup/cpu/doris"

log_stderr()
{
    echo "[`date`] $@" >&2
}

function add_workloadgroup_config()
{
    if [[ "x$ENABLE_WORKLOAD_GROUP" == "xtrue" ]]; then
          echo "doris_cgroup_cpu_path=$WORKLOAD_GROUP_PATH" >> ${DORIS_HOME}/conf/be.conf
    fi
}

update_conf_from_configmap()
{
    echo "deploy_mode = cloud" >> $DORIS_HOME/conf/be.conf
    if [[ "x$CONFIGMAP_MOUNT_PATH" == "x" ]] ; then
        log_stderr '[info] Empty $CONFIGMAP_MOUNT_PATH env var, skip it!'
        return 0
    fi
    if ! test -d $CONFIGMAP_MOUNT_PATH ; then
        log_stderr "[info] $CONFIGMAP_MOUNT_PATH not exist or not a directory, ignore ..."
        return 0
    fi
    local tgtconfdir=$DORIS_HOME/conf
    for conffile in `ls $CONFIGMAP_MOUNT_PATH`
    do
        log_stderr "[info] Process conf file $conffile ..."
        local tgt=$tgtconfdir/$conffile
        if test -e $tgt ; then
            # make a backup
            mv -f $tgt ${tgt}.bak
        fi
        if [[ "$conffile" == "be.conf" ]]; then
             cp $CONFIGMAP_MOUNT_PATH/$conffile $DORIS_HOME/conf/$file
             echo "deploy_mode = cloud" >> $DORIS_HOME/conf/$file
             continue
         fi
        ln -sfT $CONFIGMAP_MOUNT_PATH/$conffile $tgt
    done
}

# resolve password for root
resolve_password_from_secret()
{
    if [[ -f "$AUTH_PATH/password" ]]; then
        DB_ADMIN_PASSWD=`cat $AUTH_PATH/password`
    fi
    if [[ -f "$AUTH_PATH/username" ]]; then
        DB_ADMIN_USER=`cat $AUTH_PATH/username`
    fi
}

# get all backends info to check self exist or not.
show_backends(){
    local svc=$1
    backends=`timeout 15 mysql --connect-timeout 2 -h $svc -P $FE_QUERY_PORT -uroot --skip-column-names --batch -e 'SHOW BACKENDS;' 2>&1`
    log_stderr "[info] use root no password show backends result $backends ."
    if echo $backends | grep -w "1045" | grep -q -w "28000" &>/dev/null; then
        log_stderr "[info] use username and password that configured to show backends."
        backends=`timeout 15 mysql --connect-timeout 2 -h $svc -P $FE_QUERY_PORT -u$DB_ADMIN_USER -p$DB_ADMIN_PASSWD --skip-column-names --batch -e 'SHOW BACKENDS;'`
    fi

    echo "$backends"
}

# get all registered fe in cluster, for check the fe have `MASTER`.
function show_frontends()
{
    local addr=$1
    frontends=`timeout 15 mysql --connect-timeout 2 -h $addr -P $FE_QUERY_PORT -uroot --batch -e 'show frontends;' 2>&1`
    log_stderr "[info] use root no password show frontends result $frontends ."
    if echo $frontends | grep -w "1045" | grep -q -w "28000" &>/dev/null; then
        log_stderr "[info] use username and passwore that configured to show frontends."
        frontends=`timeout 15 mysql --connect-timeout 2 -h $addr -P $FE_QUERY_PORT -u$DB_ADMIN_USER -p$DB_ADMIN_PASSWD --batch -e 'show frontends;'`
    fi

    echo "$frontends"
}

#parse the `$BE_CONFIG` file, passing the key need resolve as parameter.
parse_confval_from_conf()
{
    # a naive script to grep given confkey from fe conf file
    # assume conf format: ^\s*<key>\s*=\s*<value>\s*$
    local confkey=$1
    local confvalue=`grep "\<$confkey\>" $BE_CONFIG | grep -v '^\s*#' | sed 's|^\s*'$confkey'\s*=\s*\(.*\)\s*$|\1|g'`
    echo "$confvalue"
}

collect_env_info()
{
    # heartbeat_port from conf file
    local heartbeat_port=`parse_confval_from_conf "heartbeat_service_port"`
    if [[ "x$heartbeat_port" != "x" ]] ; then
        HEARTBEAT_PORT=$heartbeat_port
    fi

    if [[ "x$HOST_TYPE" == "xIP" ]] ; then
        MY_SELF=$MY_IP
    else
        MY_SELF=$MY_HOSTNAME
    fi
}

add_self()
{
    local svc=$1
    start=`date +%s`
    local timeout=$PROBE_TIMEOUT

    while true
    do
        memlist=`show_backends $svc`
        # check ip exist
        if echo "$memlist" | grep -q -w "$MY_IP" &>/dev/null ; then
            log_stderr "[info] Check myself ($MY_IP:$HEARTBEAT_PORT) exist in FE, start be directly ..."
            break;
        fi
        if echo "$memlist" | grep -q -w "$MY_HOSTNAME" &>/dev/null ; then
            log_stderr "[info] Check myself ($MY_HOSTNAME:$HEARTBEAT_PORT) exist in FE, start be directly ..."
            break;
        fi

        # check fe cluster have master, if fe have not master wait.
        fe_memlist=`show_frontends $svc`
        local pos=`echo "$fe_memlist" | grep '\<IsMaster\>' | awk -F '\t' '{for(i=1;i<NF;i++) {if ($i == "IsMaster") print i}}'`
        local leader=`echo "$fe_memlist" | grep '\<FOLLOWER\>' | awk -v p="$pos" -F '\t' '{if ($p=="true") print $2}'`
        log_stderr "'IsMaster' sequence in columns is $pos master=$leader ."

        if [[ "x$leader" == "x" ]]; then
            log_stderr "[info] resolve the eighth column for finding master !"
            leader=`echo "$fe_memlist" | grep '\<FOLLOWER\>' | awk -F '\t' '{if ($8=="true") print $2}'`
        fi
        if [[ "x$leader" == "x" ]]; then
            # compatible 2.1.0
            log_stderr "[info] resoluve the ninth column for finding master!"
            leader=`echo "$fe_memlist" | grep '\<FOLLOWER\>' | awk -F '\t' '{if ($9=="true") print $2}'`
        fi

        if [[ "x$leader" != "x" ]]; then
            local add_sql="ALTER SYSTEM ADD BACKEND \"$MY_SELF:$HEARTBEAT_PORT\""
            if [[ $COMPUTE_GROUP_NAME != "" ]]; then
                add_sql=$add_sql" properties(\"tag.compute_group_name\"=\"$COMPUTE_GROUP_NAME\");"
            fi
            create_account $leader
            log_stderr "[info] myself ($MY_SELF:$HEARTBEAT_PORT)  not exist in FE and fe have leader register myself into fe."
            add_result=`timeout 15 mysql --connect-timeout 2 -h $svc -P $FE_QUERY_PORT -uroot --skip-column-names --batch -e "$add_sql;" 2>&1`
            if echo $add_result | grep -w "1045" | grep -q -w "28000" &>/dev/null ; then
                timeout 15 mysql --connect-timeout 2 -h $svc -P $FE_QUERY_PORT -u$DB_ADMIN_USER -p$DB_ADMIN_PASSWD --skip-column-names --batch -e "$add_sql;"
            fi

            let "expire=start+timeout"
            now=`date +%s`
            if [[ $expire -le $now ]] ; then
                log_stderr "[error]  exit probe master for probing timeout."
                return 0
            fi
        else
            log_stderr "[info] not have leader wait fe cluster elect a master, sleep 2s..."
            sleep $PROBE_INTERVAL
        fi
    done
}

function create_account()
{
    master=$1
    users=`mysql --connect-timeout 2 -h $master -P $FE_QUERY_PORT -uroot --skip-column-names --batch -e 'SHOW ALL GRANTS;' 2>&1`
    if echo $users | grep -w "1045" | grep -q -w "28000" &>/dev/null; then
        log_stderr "the 'root' account have set password! not need auto create management account."
        return 0
    fi
    if echo $users | awk '{print $1}' | grep -q -w "$DB_ADMIN_USER" &>/dev/null; then
       log_stderr "the $DB_ADMIN_USER have exist in doris."
       return 0
    fi
    mysql --connect-timeout 2 -h $master -P$FE_QUERY_PORT -uroot --skip-column-names --batch -e "CREATE USER '$DB_ADMIN_USER' IDENTIFIED BY '$DB_ADMIN_PASSWD';GRANT NODE_PRIV ON *.*.* TO $DB_ADMIN_USER;" 2>&1
    log_stderr "created new account and grant NODE_PRIV!"

}

# check be exist or not, if exist return 0, or register self in fe cluster. when all fe address failed exit script.
# `xxx1:port,xxx2:port` as parameter to function.
function check_and_register()
{
    addrs=$1
    local addrArr=(${addrs//,/ })
    for addr in ${addrArr[@]}
    do
        add_self $addr

        if [[ $REGISTERED ]]; then
            break;
        fi
    done

    if [[ $REGISTERED ]]; then
        return 0
    else
        log_stderr  "not find master in fe cluster, please use mysql connect to fe for verfing the master exist and verify domain connectivity with two pods in different node. "
        exit 1
    fi
}


function make_dir_for_workloadgroup() {
    output=$(cat /proc/filesystems | grep cgroup)
    if [ -z "$output" ]; then
        log_stderr "[error] The host machine does not have cgroup installed, so the workload group function will be limited."
        exit 1
    fi

    mkdir -p /sys/fs/cgroup/cpu/doris
    chmod 770 /sys/fs/cgroup/cpu/doris
    chown -R root:root /sys/fs/cgroup/cpu/doris

    if [[ -f "/sys/fs/cgroup/cgroup.controllers" ]]; then
        log_stderr "[info] The host machine cgroup version: v2."
        chmod a+w /sys/fs/cgroup/cgroup.procs
    else
        log_stderr "[info] The host machine cgroup version: v1."
    fi
}

mount_kerberos_config()
{
    if [[ ! -n "$KRB5_MOUNT_PATH" ]]; then
        return
    fi

    KRB5_CONFIG_DIR=$(dirname "$KRB5_CONFIG")
    # If the krb5 directory does not exist, need to create it.
    if [[ ! -d "$KRB5_CONFIG_DIR" ]]; then
        log_stderr "[info] Creating krb5 directory: $KRB5_CONFIG_DIR"
        mkdir -p "$KRB5_CONFIG_DIR"
    fi

    log_stderr "[info] Creating krb5 symlinks for each file from $KRB5_MOUNT_PATH to $KRB5_CONFIG_DIR"
    # The files under KRB5_MONT_PATH are soft links from other directories. Therefore, a for loop is needed to directly soft link the files.
    for file in "$KRB5_MOUNT_PATH"/*; do
        if [ -e "$file" ]; then
            filename=$(basename "$file")
            log_stderr "[info] Creating krb5 symlink for $filename"
            ln -sf "$file" "$KRB5_CONFIG_DIR/$filename"
        fi
    done

    if [[ "$KEYTAB_MOUNT_PATH" == "$KEYTAB_FINAL_USED_PATH" ]]; then
        log_stderr "[info] KEYTAB_MOUNT_PATH is same as KEYTAB_FINAL_USED_PATH, skip creating symlinks"
        return
    fi

    # If the keytab directory does not exist, need to create it.
    if [[ ! -d "$KEYTAB_FINAL_USED_PATH" ]]; then
        log_stderr "[info] Creating keytab directory: $KEYTAB_FINAL_USED_PATH"
        mkdir -p "$KEYTAB_FINAL_USED_PATH"
    fi

    log_stderr "[info] Creating keytab symlinks for each file from $KEYTAB_MOUNT_PATH to $KEYTAB_FINAL_USED_PATH"
    # The files under KEYTAB_MOUNT_PATH are soft links from other directories. Therefore, a for loop is needed to directly soft link the files.
    for file in "$KEYTAB_MOUNT_PATH"/*; do
        if [ -e "$file" ]; then
            filename=$(basename "$file")
            log_stderr "[info] Creating keytab symlink for $filename"
            ln -sf "$file" "$KEYTAB_FINAL_USED_PATH/$filename"
        fi
    done
}


# scripts start position.
fe_addrs=$1
if [[ "x$fe_addrs" == "x" ]]; then
    echo "need fe address as paramter!"
    echo "  Example $0 <fe_addr>"
    exit 1
fi

if [[ "x$ENABLE_WORKLOAD_GROUP" == "xtrue" ]]; then
      log_stderr '[info] Enable workload group !'
      make_dir_for_workloadgroup
fi


# pre check for starting
if cat /proc/cpuinfo | grep -q "avx2" &>/dev/null; then
    log_stderr "[info] the host machine support avx2 instruction set."
else
    log_stderr "[info] the host machine not support avx2 instruction set."
fi

update_conf_from_configmap
add_workloadgroup_config
mount_kerberos_config
# resolve password for root to manage nodes in doris.
resolve_password_from_secret
collect_env_info
#add_self $fe_addr || exit $?
check_and_register $fe_addrs
./doris-debug --component be
log_stderr "run start_be.sh"
# the server will start in the current terminal session, and the log output and console interaction will be printed to that terminal
# sine doris 2.0.2 ,doris start with : start_xx.sh --console  doc: https://doris.apache.org/docs/dev/install/standard-deployment/#version--202
$DORIS_HOME/bin/start_be.sh --console
