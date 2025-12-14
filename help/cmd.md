cd main

# 0) 安装工具（只需一次）
pip install esp-secure-cert-tool

# 1) 生成 RSA 私钥 + CSR
openssl genrsa -out client.key 2048
openssl req -new -key client.key -out client.csr

# 2) 准备设备证书（client.crt）
# - 推荐：把 client.csr 提交到 Mosquitto test signer，下载得到 client.crt 放在当前目录
# - 或临时自签（不一定被服务器信任）：
openssl x509 -req -in client.csr -signkey client.key -out client.crt -days 365 -sha256

# 3) 配置 DS + 生成 main/esp_secure_cert_data/esp_secure_cert.bin
configure_esp_secure_cert.py -p /dev/tty.usbmodem1101 --device-cert client.crt --private-key client.key --target_chip esp32s3 --configure_ds --skip_flash --priv_key_algo RSA 2048 --efuse_key_id 1

# 4) 烧录用扩展的就行
