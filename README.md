# Camera Stream Manager

桌面目录直接运行：./run_camera_stream_manager.sh

说明：
1. 默认通过跳板机 root@192.168.10.22 转发后，再连接 nvidia@车IP。
2. 远端配置目录默认是 /breton_pilot/modules/drivers/camera/config。
3. 远端日志目录默认是 /breton_data/logs/camera。
4. 本地日志默认写到当前工具目录下的 logs。
5. 只会修改这 5 个摄像头：camera_front_left、camera_ir_left、camera_surround_left_rear、camera_surround_right_rear、camera_rear_left。
6. camera_ir_left 只改 stream_bitrate，其余 4 个改 stream_bitrate、stream_width、stream_height。

配置文件：
- vehicles.conf：跳板机、远端用户、日志目录、车号与 IP。
- camera_import_template.json：JSON 导入模板。

补充：
- 如果某台车密码不同，可以把 vehicles.conf 里的单行改成 160=192.168.12.160,你的密码。
- 如果不需要跳板机，把 jump_host_ip 和 jump_host_password 清空即可。
