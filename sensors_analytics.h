/*
 * Copyright (C) 2015 SensorsData 
 * All rights reserved.
 */

#ifndef SENSORS_ANALYTICS_CORE_H
#define SENSORS_ANALYTICS_CORE_H

#include <stdlib.h>
#include <time.h>

// SDK 错误码
typedef enum {
    SA_OK,
    SA_MALLOC_ERROR,
    SA_INVALID_PARAMETER_ERROR,
    SA_IO_ERROR
} SAErrCode;

typedef enum { 
    SA_FALSE,
    SA_TRUE
} SABool;

// ----------------------------------------------------------------------------

// 定义 Consumer 的操作
typedef int (*sa_consumer_send)(void* this_, const char* event, unsigned long length);
typedef int (*sa_consumer_flush)(void* this_);
typedef int (*sa_consumer_close)(void* this_);

struct SAConsumerOp {
    sa_consumer_send send;
    sa_consumer_flush flush;
    sa_consumer_close close;
};

struct SAConsumer {
    struct SAConsumerOp op;
    // Consumer 私有数据
    void* this_;
};

// LoggingConsumer 用于将事件以日志文件的形式记录在本地磁盘中
typedef struct SAConsumer SALoggingConsumer;

// 初始化 Logging Consumer
//
// @param file_name<in>    日志文件名，例如: /data/logs/http.log
// @param consumer<out>    SALoggingConsumer 实例
//
// @return SA_OK 初始化成功，否则初始化失败
int sa_init_logging_consumer(const char* file_name, SALoggingConsumer** consumer);

// DebugConsumer 用于在线调试 SDK 记录的数据
typedef struct SAConsumer SADebugConsumer;

// 初始化 DebugConsumer
//  
// @param url<in>          Sensors Analytics 采集数据的 URL
// @param write_data<in>   Debug 模式下是否将调试数据写入 Sensors Analytics，
//                         SA_TRUE - 写入，SA_FALSE - 不写入
// @param consumer<out>    SADebugConsumer 实例
//
// @return SA_OK 初始化成功，否则初始化失败
int sa_init_debug_consumer(const char* url, SABool write_data, SADebugConsumer** consumer);

// BatchConsumer 用于在内网批量发送本地数据至私有部署的 Sensors Analytics
typedef struct SAConsumer SABatchConsumer;

// 初始化 BatchConsumer
//
// @param url<in>          Sensors Analytics 采集数据的 URL
// @param batch_size<in>   批量发送的数据条目数，最大为 100
// @param consumer<out>    SABatchConsumer 实例
//
// @return SA_OK 初始化成功，否则初始化失败
int sa_init_batch_consumer(const char* url, unsigned int batch_size, SABatchConsumer** consumer);

// ----------------------------------------------------------------------------

// SensorsAnalytics 对象
typedef struct SensorsAnalytics SensorsAnalytics;

// 初始化 Sensors Analytics 对象
//
// @param consumer<in>         日志数据的“消费”方式
// @param sa<out>              初始化的 Sensors Analytics 实例
//
// @return SA_OK 初始化成功，否则初始化失败
int sa_init(struct SAConsumer* consumer, struct SensorsAnalytics** sa);

// 释放 Sensors Analytics 对象
//
// @param sa<in/out>           释放的 Sensors Analytics 实例
void sa_free(struct SensorsAnalytics* sa);

// 同步 Sensors Analytics 的状态，将发送 Consumer 的缓存中所有数据
//
// @param sa<in/out>           同步的 Sensors Analytics 实例
void sa_flush(struct SensorsAnalytics* sa);

// ----------------------------------------------------------------------------

// 事件属性或用户属性 
typedef struct SANode SAProperties;

// 初始化事件属性或用户属性对象
//
// @return SAProperties 对象，NULL表示初始化失败
SAProperties* sa_init_properties();

// 释放事件属性或用户属性对象
//
// @param properties<out>   被释放的 SAProperties 对象
void sa_free_properties(SAProperties* properties);

// 向事件属性或用户属性添加 Bool 类型的属性
//
// @param key<in>           属性名称
// @param bool_<in>         SABool 对象，属性值
// @param properties<out>   SAProperties 对象
//
// @return SA_OK 添加成功，否则失败
int sa_add_bool(const char* key, SABool bool_, SAProperties* properties); 

// 向事件属性或用户属性添加 Number 类型的属性
//
// @param key<in>           属性名称
// @param number_<in>       属性值
// @param properties<out>   SAProperties 对象
//
// @return SA_OK 添加成功，否则失败
int sa_add_number(const char* key, double number_, SAProperties* properties);

// 向事件属性或用户属性添加 Date 类型的属性
//
// @param key<in>           属性名称
// @param seconds<in>       时间戳，单位为秒
// @param microseconds<in>  时间戳中毫秒部分
// @param properties<out>   SAProperties 对象
//
// @return SA_OK 添加成功，否则失败
int sa_add_date(const char* key, time_t seconds, int microseconds, SAProperties* properties);

// 向事件属性或用户属性添加 String 类型的属性
//
// @param key<in>           属性名称
// @param string_<in>       字符串的句柄
// @param length<in>        字符串长度
// @param properties<out>   SAProperties 对象
//
// @return SA_OK 添加成功，否则失败
int sa_add_string(
        const char* key, 
        const char* string_, 
        unsigned int length, 
        SAProperties* properties);

// 向事件属性或用户属性的 List 类型的属性中插入新对象，对象必须是 String 类型的
//
// @param key<in>           属性名称
// @param string_<in>       字符串的句柄
// @param length<in>        字符串长度
// @param properties<out>   SAProperties 对象
//
// @return SA_OK 添加成功，否则失败
int sa_append_list(
        const char* key, 
        const char* string_, 
        unsigned int length, 
        SAProperties* properties);

// 设置事件的一些公共属性，当 track 的 properties 和 super properties 有相同的 key 时，将采用
// track 的
int sa_register_super_properties(const SAProperties* properties, struct SensorsAnalytics* sa);
// 删除事件的一个公共属性
int sa_unregister_super_properties(const char* key, struct SensorsAnalytics* sa);
// 删除事件的所有公共属性
int sa_clear_super_properties(struct SensorsAnalytics* sa);

// 跟踪一个用户的行为
// 
// @param distinct_id<in>      用户ID
// @param event<in>            事件名称
// @param properties<in>       事件属性，SAProperties 对象，NULL 表示无事件属性
// @param sa<in/out>           SensorsAnalytics 实例
//
// @return SA_OK 追踪成功，否则追踪失败
#define sa_track(distinct_id, event, properties, sa)      \
    _sa_track(distinct_id, event, properties, __FILE__, __FUNCTION__, __LINE__, sa)
int _sa_track(
        const char* distinct_id, 
        const char* event, 
        const SAProperties* properties, 
        const char* __file__,
        const char* __function__,
        unsigned long __line__,
        struct SensorsAnalytics* sa);


// 关联匿名用户和注册用户，这个接口是一个较为复杂的功能，请在使用前先阅读相关说明:
//
//   http://www.sensorsdata.cn/manual/track_signup.html
//
// 并在必要时联系我们的技术支持人员。
//
// @param distinct_id<in>       用户的注册 ID
// @param origin_id<in>         被关联的用户匿名 ID
// @param properties<in>        事件属性，NULL 表示无事件属性
// @param sa<in/out>            SensorsAnalytics 对象
//
// @return SA_OK 追踪关联事件成功，否则失败
#define sa_track_signup(distinct_id, origin_id, properties, sa)   \
    _sa_track_signup(distinct_id, origin_id, properties, __FILE__, __FUNCTION__, __LINE__, sa)
int _sa_track_signup(
        const char* distinct_id, 
        const char* origin_id, 
        const SAProperties* properties,
        const char* __file__,
        const char* __function__,
        unsigned long __line__,
        struct SensorsAnalytics* sa);

// 设置用户属性，如果某个属性已经在该用户的属性中存在，则覆盖原有属性
//
// @param distinct_id<in>       用户 ID
// @param properties<in>        用户属性
// @param sa<in/out>            SensorsAnalytics 对象
//
// @return SA_OK 设置成功，否则失败
#define sa_profile_set(distinct_id, properties, sa)   \
    _sa_profile_set(distinct_id, properties, __FILE__, __FUNCTION__, __LINE__, sa)
int _sa_profile_set(
        const char* distinct_id,
        const SAProperties* properties,
        const char* __file__,
        const char* __function__,
        unsigned long __line__,
        struct SensorsAnalytics* sa);

// 设置用户属性，如果某个属性已经在该用户的属性中存在，则不设置该属性
//
// @param distinct_id<in>       用户 ID
// @param properties<in>        用户属性
// @param sa<in/out>            SensorsAnalytics 对象
//
// @return SA_OK 设置成功，否则失败
#define sa_profile_set_once(distinct_id, properties, sa)   \
    _sa_profile_set_once(distinct_id, properties, __FILE__, __FUNCTION__, __LINE__, sa)
int _sa_profile_set_once(
        const char* distinct_id,
        const SAProperties* properties,
        const char* __file__,
        const char* __function__,
        unsigned long __line__,
        struct SensorsAnalytics* sa);

// 增加或减少用户属性中的 Number 类型属性的值
//
// @param distinct_id<in>       用户 ID
// @param properties<in>        用户属性，必须为 Number 类型的属性 
// @param sa<in/out>            SensorsAnalytics 对象
//
// @return SA_OK 设置成功，否则失败
#define sa_profile_increment(distinct_id, properties, sa)   \
    _sa_profile_increment(distinct_id, properties, __FILE__, __FUNCTION__, __LINE__, sa)
int _sa_profile_increment(
        const char* distinct_id,
        const SAProperties* properties,
        const char* __file__,
        const char* __function__,
        unsigned long __line__,
        struct SensorsAnalytics* sa);

// 向用户属性中的 List 属性增加新元素
//
// @param distinct_id<in>       用户 ID
// @param properties<in>        用户属性，必须为 List 类型的属性 
// @param sa<in/out>            SensorsAnalytics 对象
//
// @return SA_OK 设置成功，否则失败
#define sa_profile_append(distinct_id, properties, sa)   \
    _sa_profile_append(distinct_id, properties, __FILE__, __FUNCTION__, __LINE__, sa)
int _sa_profile_append(
        const char* distinct_id,
        const SAProperties* properties,
        const char* __file__,
        const char* __function__,
        unsigned long __line__,
        struct SensorsAnalytics* sa);

// 删除某用户的一个属性
//
// @param distinct_id<in>       用户 ID
// @param key<in>               用户属性名称
// @param sa<in/out>            SensorsAnalytics 对象
//
// @return SA_OK 设置成功，否则失败
#define sa_profile_unset(distinct_id, key, sa)   \
    _sa_profile_unset(distinct_id, key, __FILE__, __FUNCTION__, __LINE__, sa)
int _sa_profile_unset(
        const char* distinct_id,
        const char* key,
        const char* __file__,
        const char* __function__,
        unsigned long __line__,
        struct SensorsAnalytics* sa);

// 删除某用户所有属性
//
// @param distinct_id<in>       用户 ID
// @param sa<in/out>            SensorsAnalytics 对象
//
// @return SA_OK 设置成功，否则失败
#define sa_profile_delete(distinct_id, sa)   \
    _sa_profile_delete(distinct_id, __FILE__, __FUNCTION__, __LINE__, sa)
int _sa_profile_delete(
        const char* distinct_id,
        const char* __file__,
        const char* __function__,
        unsigned long __line__,
        struct SensorsAnalytics* sa);


#endif  // SENSORS_ANALYTICS_CORE_H
