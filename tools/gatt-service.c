/*
 *
 *  BlueZ - Bluetooth protocol stack for Linux
 *
 *  Copyright (C) 2014  Instituto Nokia de Tecnologia - INdT
 *
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <unistd.h>
#include <string.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>

#include <glib.h>
#include <dbus/dbus.h>

#include "gdbus/gdbus.h"

#include "src/error.h"

#define GATT_MGR_IFACE			"org.bluez.GattManager1"
#define GATT_SERVICE_IFACE		"org.bluez.GattService1"
#define GATT_CHR_IFACE			"org.bluez.GattCharacteristic1"
#define GATT_DESCRIPTOR_IFACE		"org.bluez.GattDescriptor1"

/* Immediate wifi Service UUID */
#define WIFI_SERVICES_UUID       "1B7E8251-2877-41C3-B46E-CF057C562023"
#define SECURITY_UUID            "CAC2ABA4-EDBB-4C4A-BBAF-0A84A5CD93A1"
#define SSID_UUID                "ACA0EF7C-EEAA-48AD-9508-19A6CEF6B356"
#define PASSWORD_UUID            "40B7DE33-93E4-4C8B-A876-D833B415A6CE"
#define CONFIG_NOTIFY_UUID       "8AC32D3f-5CB9-4D44-BEC2-EE689169F626"
#define CONFIG_UUID              "00002902-0000-1000-8000-00805f9b34fb"

#ifdef DUEROS
#define DUEROS_WIFI_SERVICES_UUID       "00001111-0000-1000-8000-00805f9b34fb"
#define DUEROS_CHARACTERISTIC_UUID      "00002222-0000-1000-8000-00805f9b34fb"

#define DUEROS_SOCKET_RECV_LEN 20

static pthread_t dueros_tid = 0;
static int dueros_socket_done = 0;
static int dueros_socket_fd = -1;
static char dueros_socket_path[] = "/data/bluez5_utils/socket_dueros";

struct characteristic *dueros_chr;

typedef struct{
    int server_sockfd;
    int client_sockfd;
    int server_len;
    int client_len;
    struct sockaddr_un server_address;
    struct sockaddr_un client_address;
    char sock_path[512];
} tAPP_SOCKET;

#define CMD_ADV                  "hcitool -i hci0 cmd 0x08 0x0008 15 02 01 06 11 07 fb 34 9b 5f 80 00 00 80 00 10 00 00 11 11 00 00"
#else
#define CMD_ADV                  "hcitool -i hci0 cmd 0x08 0x0008 15 02 01 06 11 07 23 20 56 7c 05 cf 6e b4 c3 41 77 28 51 82 7e 1b"
#endif

#define CMD_EN                   "hcitool -i hci0 cmd 0x08 0x000a 1"

#define ADV_IRK		"\x69\x30\xde\xc3\x8f\x84\x74\x14" \

const char *WIFI_CONFIG_FORMAT = "ctrl_interface=/var/run/wpa_supplicant\n"
                                "ap_scan=1\n\nnetwork={\nssid=\"%s\"\n"
                                "psk=\"%s\"\npriority=1\n}\n";

static GMainLoop *main_loop;
static GSList *services;
static DBusConnection *connection;

char wifi_ssid[256];
char wifi_password[256];

struct characteristic {
	char *service;
	char *uuid;
	char *path;
	uint8_t *value;
	int vlen;
	const char **props;
};

struct descriptor {
	struct characteristic *chr;
	char *uuid;
	char *path;
	uint8_t *value;
	int vlen;
	const char **props;
};

/*
 * Supported properties are defined at doc/gatt-api.txt. See "Flags"
 * property of the GattCharacteristic1.
 */
//static const char *chr_props[] = { "read", "write", "notify", "indicate", NULL };
static const char *chr_props[] = { "read", "write", "notify", NULL };
static const char *desc_props[] = { "read", "write", NULL };

static void chr_write(struct characteristic *chr, const uint8_t *value, int len);
static void chr_iface_destroy(gpointer user_data);

#ifdef DUEROS
/********************SERVER API***************************/
static int setup_socket_server(tAPP_SOCKET *app_socket)
{
    unlink (app_socket->sock_path);
    if ((app_socket->server_sockfd = socket (AF_UNIX, SOCK_STREAM, 0)) < 0) {
        printf("fail to create socket\n");
        perror("socket");
        return -1;
    }
    app_socket->server_address.sun_family = AF_UNIX;
    strcpy (app_socket->server_address.sun_path, app_socket->sock_path);
    app_socket->server_len = sizeof (app_socket->server_address);
    app_socket->client_len = sizeof (app_socket->client_address);
    if ((bind (app_socket->server_sockfd, (struct sockaddr *)&app_socket->server_address, app_socket->server_len)) < 0) {
        perror("bind");
        return -1;

    }
    if (listen (app_socket->server_sockfd, 10) < 0) {
        perror("listen");
        return -1;
    }
    printf ("Server is ready for client connect...\n");

    return 0;
}

static int accpet_client(tAPP_SOCKET *app_socket)
{
    app_socket->client_sockfd = accept (app_socket->server_sockfd, (struct sockaddr *)&app_socket->server_address, (socklen_t *)&app_socket->client_len);
    if (app_socket->client_sockfd == -1) {
        perror ("accept");
        return -1;
    }
    return 0;
}

static void teardown_socket_server(tAPP_SOCKET *app_socket)
{
    unlink (app_socket->sock_path);
    app_socket->server_sockfd = 0;
    app_socket->client_sockfd = 0;
}

/********************CLIENT API***************************/
static int setup_socket_client(char *socket_path)
{
    struct sockaddr_un address;
    int sockfd,  len;

    if ((sockfd = socket(AF_UNIX, SOCK_STREAM, 0)) == -1) {
            printf("%s: can not creat socket\n", __func__);
            return -1;
    }

    address.sun_family = AF_UNIX;
    strcpy (address.sun_path, socket_path);
    len = sizeof (address);

    if (connect (sockfd, (struct sockaddr *)&address, len) == -1) {
        printf("%s: can not connect to socket\n", __func__);;
        return -1;
    }

    return sockfd;

}

static void teardown_socket_client(int sockfd)
{
    close(sockfd);
}

/********************COMMON API***************************/
static int socket_send(int sockfd, char *msg, int len)
{
    int bytes;
    if (sockfd < 0) {
        printf("%s: invalid sockfd\n",__func__);
        return -1;
    }
    if ((bytes = send(sockfd, msg, len, 0)) < 0) {
        perror ("send");
    }
    return bytes;
}

static int socket_recieve(int sockfd, char *msg, int len)
{
    int bytes;

    if (sockfd < 0) {
        printf("%s: invalid sockfd\n",__func__);
        return -1;
    }

    if ((bytes = recv(sockfd, msg, len, 0)) < 0)
    {
        perror ("recv");
    }
    return bytes;

}

static int dueros_socket_send(char *msg, int len) {
    return socket_send(dueros_socket_fd, msg, len);
}

static void *dueros_socket_recieve(void *arg) {
    int bytes = 0;
    char data[DUEROS_SOCKET_RECV_LEN];

    dueros_socket_fd = setup_socket_client(dueros_socket_path);
    if (dueros_socket_fd < 0) {
        printf("Fail to connect server socket\n");
        goto exit;
    }

    while (dueros_socket_done) {
        memset(data, 0, sizeof(data));
        bytes = socket_recieve(dueros_socket_fd, data, sizeof(data));
        if (bytes <= 0) {
            printf("Server leaved, break\n");
            break;
        }

        printf("dueros_socket_recieve, len: %d\n", bytes);
        for(int i = 0; i < bytes; i++)
            printf("%02x ", data[i]);
        printf("\n\n");

        //send to apk
        chr_write(dueros_chr, (uint8_t *) data, bytes);
        usleep(1000000); //sleep 1s
    }

exit:
    printf("Exit dueros socket thread\n");
    pthread_exit(0);
}

static int dueros_socket_thread_create(void) {
    dueros_socket_done = 1;
    if (pthread_create(&dueros_tid, NULL, dueros_socket_recieve, NULL)) {
        printf("Create dueros socket thread failed\n");
        return -1;
    }

    return 0;
}

static void dueros_socket_thread_delete(void) {
    dueros_socket_done = 0;

    teardown_socket_client(dueros_socket_fd);

    if (dueros_tid) {
        pthread_join(dueros_tid, NULL);
        dueros_tid = 0;
    }
}

#endif

static gboolean desc_get_uuid(const GDBusPropertyTable *property,
					DBusMessageIter *iter, void *user_data)
{
	struct descriptor *desc = user_data;

	dbus_message_iter_append_basic(iter, DBUS_TYPE_STRING, &desc->uuid);

	return TRUE;
}

static gboolean desc_get_characteristic(const GDBusPropertyTable *property,
					DBusMessageIter *iter, void *user_data)
{
	struct descriptor *desc = user_data;

	dbus_message_iter_append_basic(iter, DBUS_TYPE_OBJECT_PATH,
						&desc->chr->path);

	return TRUE;
}

static bool desc_read(struct descriptor *desc, DBusMessageIter *iter)
{
	DBusMessageIter array;

	dbus_message_iter_open_container(iter, DBUS_TYPE_ARRAY,
					DBUS_TYPE_BYTE_AS_STRING, &array);

	if (desc->vlen && desc->value)
		dbus_message_iter_append_fixed_array(&array, DBUS_TYPE_BYTE,
						&desc->value, desc->vlen);

	dbus_message_iter_close_container(iter, &array);

	return true;
}

static gboolean desc_get_value(const GDBusPropertyTable *property,
					DBusMessageIter *iter, void *user_data)
{
	struct descriptor *desc = user_data;

	printf("Descriptor(%s): Get(\"Value\")\n", desc->uuid);

	return desc_read(desc, iter);
}

static void desc_write(struct descriptor *desc, const uint8_t *value, int len)
{
	g_free(desc->value);
	desc->value = g_memdup(value, len);
	desc->vlen = len;

	g_dbus_emit_property_changed(connection, desc->path,
					GATT_DESCRIPTOR_IFACE, "Value");
}

static int parse_value(DBusMessageIter *iter, const uint8_t **value, int *len)
{
	DBusMessageIter array;

	if (dbus_message_iter_get_arg_type(iter) != DBUS_TYPE_ARRAY)
		return -EINVAL;

	dbus_message_iter_recurse(iter, &array);
	dbus_message_iter_get_fixed_array(&array, value, len);

	return 0;
}

static void desc_set_value(const GDBusPropertyTable *property,
				DBusMessageIter *iter,
				GDBusPendingPropertySet id, void *user_data)
{
	struct descriptor *desc = user_data;
	const uint8_t *value;
	int len;

	printf("Descriptor(%s): Set(\"Value\", ...)\n", desc->uuid);

	if (parse_value(iter, &value, &len)) {
		printf("Invalid value for Set('Value'...)\n");
		g_dbus_pending_property_error(id,
					ERROR_INTERFACE ".InvalidArguments",
					"Invalid arguments in method call");
		return;
	}

	desc_write(desc, value, len);

	g_dbus_pending_property_success(id);
}

static gboolean desc_get_props(const GDBusPropertyTable *property,
					DBusMessageIter *iter, void *data)
{
	struct descriptor *desc = data;
	DBusMessageIter array;
	int i;

	dbus_message_iter_open_container(iter, DBUS_TYPE_ARRAY,
					DBUS_TYPE_STRING_AS_STRING, &array);

	for (i = 0; desc->props[i]; i++)
		dbus_message_iter_append_basic(&array,
					DBUS_TYPE_STRING, &desc->props[i]);

	dbus_message_iter_close_container(iter, &array);

	return TRUE;
}

static const GDBusPropertyTable desc_properties[] = {
	{ "UUID",		"s", desc_get_uuid },
	{ "Characteristic",	"o", desc_get_characteristic },
	{ "Value",		"ay", desc_get_value, desc_set_value, NULL },
	{ "Flags",		"as", desc_get_props, NULL, NULL },
	{ }
};

static gboolean chr_get_uuid(const GDBusPropertyTable *property,
					DBusMessageIter *iter, void *user_data)
{
	struct characteristic *chr = user_data;

	dbus_message_iter_append_basic(iter, DBUS_TYPE_STRING, &chr->uuid);

	return TRUE;
}

static gboolean chr_get_service(const GDBusPropertyTable *property,
					DBusMessageIter *iter, void *user_data)
{
	struct characteristic *chr = user_data;

	dbus_message_iter_append_basic(iter, DBUS_TYPE_OBJECT_PATH,
							&chr->service);

	return TRUE;
}

static bool chr_read(struct characteristic *chr, DBusMessageIter *iter)
{
	DBusMessageIter array;

	dbus_message_iter_open_container(iter, DBUS_TYPE_ARRAY,
					DBUS_TYPE_BYTE_AS_STRING, &array);

	dbus_message_iter_append_fixed_array(&array, DBUS_TYPE_BYTE,
						&chr->value, chr->vlen);

	dbus_message_iter_close_container(iter, &array);

	return true;
}

static gboolean chr_get_value(const GDBusPropertyTable *property,
					DBusMessageIter *iter, void *user_data)
{
	struct characteristic *chr = user_data;

	printf("Characteristic(%s): Get(\"Value\")\n", chr->uuid);

	return chr_read(chr, iter);
}

static gboolean chr_get_props(const GDBusPropertyTable *property,
					DBusMessageIter *iter, void *data)
{
	struct characteristic *chr = data;
	DBusMessageIter array;
	int i;

	dbus_message_iter_open_container(iter, DBUS_TYPE_ARRAY,
					DBUS_TYPE_STRING_AS_STRING, &array);

	for (i = 0; chr->props[i]; i++)
		dbus_message_iter_append_basic(&array,
					DBUS_TYPE_STRING, &chr->props[i]);

	dbus_message_iter_close_container(iter, &array);

	return TRUE;
}

static void chr_write(struct characteristic *chr, const uint8_t *value, int len)
{
	g_free(chr->value);
	chr->value = g_memdup(value, len);
	chr->vlen = len;

	g_dbus_emit_property_changed(connection, chr->path, GATT_CHR_IFACE,
								"Value");
}

static void chr_set_value(const GDBusPropertyTable *property,
				DBusMessageIter *iter,
				GDBusPendingPropertySet id, void *user_data)
{
	struct characteristic *chr = user_data;
	const uint8_t *value;
	int len;

	printf("Characteristic(%s): Set('Value', ...)\n", chr->uuid);

	if (!parse_value(iter, &value, &len)) {
		printf("Invalid value for Set('Value'...)\n");
		g_dbus_pending_property_error(id,
					ERROR_INTERFACE ".InvalidArguments",
					"Invalid arguments in method call");
		return;
	}

	chr_write(chr, value, len);

	g_dbus_pending_property_success(id);
}

static const GDBusPropertyTable chr_properties[] = {
	{ "UUID",	"s", chr_get_uuid },
	{ "Service",	"o", chr_get_service },
	{ "Value",	"ay", chr_get_value, chr_set_value, NULL },
	{ "Flags",	"as", chr_get_props, NULL, NULL },
	{ }
};

static gboolean service_get_primary(const GDBusPropertyTable *property,
					DBusMessageIter *iter, void *user_data)
{
	dbus_bool_t primary = TRUE;

	printf("Get Primary: %s\n", primary ? "True" : "False");

	dbus_message_iter_append_basic(iter, DBUS_TYPE_BOOLEAN, &primary);

	return TRUE;
}

static gboolean service_get_uuid(const GDBusPropertyTable *property,
					DBusMessageIter *iter, void *user_data)
{
	const char *uuid = user_data;

	printf("Get UUID: %s\n", uuid);

	dbus_message_iter_append_basic(iter, DBUS_TYPE_STRING, &uuid);

	return TRUE;
}

static gboolean service_get_includes(const GDBusPropertyTable *property,
					DBusMessageIter *iter, void *user_data)
{
	const char *uuid = user_data;
	char service_path[100] = {0,};
	DBusMessageIter array;
	char *p = NULL;

	snprintf(service_path, 100, "/service3");
	printf("Get Includes: %s\n", uuid);

	p = service_path;

	printf("Includes path: %s\n", p);

	dbus_message_iter_open_container(iter, DBUS_TYPE_ARRAY,
			DBUS_TYPE_OBJECT_PATH_AS_STRING, &array);

	dbus_message_iter_append_basic(&array, DBUS_TYPE_OBJECT_PATH,
							&p);

	snprintf(service_path, 100, "/service2");
	p = service_path;
	printf("Get Includes: %s\n", p);

	dbus_message_iter_append_basic(&array, DBUS_TYPE_OBJECT_PATH,
							&p);
	dbus_message_iter_close_container(iter, &array);


	return TRUE;

}


static gboolean service_exist_includes(const GDBusPropertyTable *property,
							void *user_data)
{
	const char *uuid = user_data;

	printf("Exist Includes: %s\n", uuid);

#ifdef DUEROS
	if (strncmp(uuid, "00001111", 8) == 0)
		return TRUE;
#else
	if (strncmp(uuid, "1B7E8251", 8) == 0)
		return TRUE;
#endif

	return FALSE;
}

static const GDBusPropertyTable service_properties[] = {
	{ "Primary", "b", service_get_primary },
	{ "UUID", "s", service_get_uuid },
	{ "Includes", "ao", service_get_includes, NULL,
					service_exist_includes },
	{ }
};

static void chr_iface_destroy(gpointer user_data)
{
	struct characteristic *chr = user_data;

	g_free(chr->uuid);
	g_free(chr->service);
	g_free(chr->value);
	g_free(chr->path);
	g_free(chr);
}

static void desc_iface_destroy(gpointer user_data)
{
	struct descriptor *desc = user_data;

	g_free(desc->uuid);
	g_free(desc->value);
	g_free(desc->path);
	g_free(desc);
}

static int parse_options(DBusMessageIter *iter, const char **device)
{
	DBusMessageIter dict;

	if (dbus_message_iter_get_arg_type(iter) != DBUS_TYPE_ARRAY)
		return -EINVAL;

	dbus_message_iter_recurse(iter, &dict);

	while (dbus_message_iter_get_arg_type(&dict) == DBUS_TYPE_DICT_ENTRY) {
		const char *key;
		DBusMessageIter value, entry;
		int var;

		dbus_message_iter_recurse(&dict, &entry);
		dbus_message_iter_get_basic(&entry, &key);

		dbus_message_iter_next(&entry);
		dbus_message_iter_recurse(&entry, &value);

		var = dbus_message_iter_get_arg_type(&value);
		if (strcasecmp(key, "device") == 0) {
			if (var != DBUS_TYPE_OBJECT_PATH)
				return -EINVAL;
			dbus_message_iter_get_basic(&value, device);
			printf("Device: %s\n", *device);
		}

		dbus_message_iter_next(&dict);
	}

	return 0;
}

void execute(const char cmdline[],char recv_buff[]){
    printf("consule_run: %s\n",cmdline);

    FILE *stream = NULL;
    char buff[1024];

    memset(recv_buff, 0, sizeof(recv_buff));

    if((stream = popen(cmdline,"r"))!=NULL){
        while(fgets(buff,1024,stream)){
            strcat(recv_buff,buff);
        }
    }

    pclose(stream);
}

//wpa_supplicant
static int wpa_supplicant_config_wifi() {
    FILE *fp = NULL;

    if ((fp = fopen("/data/cfg/wpa_supplicant.conf", "w+")) == NULL)
    {
        printf("open cfg wpa_supplicant.conf failed");
        return -1;
    }

    fprintf(fp, "%s\n", "ctrl_interface=/var/run/wpa_supplicant");
    fprintf(fp, "%s\n", "ap_scan=1");
    fprintf(fp, "%s\n", "network={");
    fprintf(fp, "%s%s%s\n", "ssid=\"", wifi_ssid, "\"");
    fprintf(fp, "%s%s%s\n", "psk=\"", wifi_password, "\"");
    fprintf(fp, "%s\n", "key_mgmt=WPA-PSK");
    fprintf(fp, "%s\n", "}");

    fclose(fp);

    if (-1 == system("killall wpa_supplicant; dhcpcd -k wlan0; killall dhcpcd;"
                   "ifconfig wlan0 0.0.0.0")) {
        printf("killall wpa_supplicant dhcpcd failed");
        return -1;
    }

    if (-1 == system("wpa_supplicant -Dnl80211 -i wlan0 "
                   "-c /data/cfg/wpa_supplicant.conf &")) {
        printf("start wpa_supplicant failed");
        return -1;
    }

    if (-1 == system("dhcpcd wlan0 -t 0 &")) {
        printf("dhcpcd failed");
        return -1;
    }

    return 0;
}

//wpa_cli
static gboolean wpa_cli_config_wifi(){
    printf("start config_wifi\n");
    char buff[256] = {0};
    char cmdline[256] = {0};
    int id = -1;
    bool execute_result = false;

    // 1. add network
    execute("wpa_cli -iwlan0 add_network",buff);
    id = atoi(buff);
    if(id < 0){
        perror("add_network failed.\n");
        return FALSE;
    }

    // 2. setNetWorkSSID
    memset(cmdline, 0, sizeof(cmdline));
    sprintf(cmdline,"wpa_cli -iwlan0 set_network %d ssid \\\"%s\\\"",id, wifi_ssid);
    printf("%s\n", cmdline);
    execute(cmdline,buff);
    execute_result = !strncmp(buff,"OK",2);

    if(!execute_result){
        perror("setNetWorkSSID failed.\n");
        return FALSE;
    }

    // 3. setNetWorkPWD
    memset(cmdline, 0, sizeof(cmdline));
    sprintf(cmdline,"wpa_cli -iwlan0 set_network %d psk \\\"%s\\\"", id,wifi_password);
    printf("%s\n", cmdline);
    execute(cmdline,buff);
    execute_result = !strncmp(buff,"OK",2);

    if(!execute_result){
        perror("setNetWorkPWD failed.\n");
        return FALSE;
    }

    // 4. selectNetWork
    memset(cmdline, 0, sizeof(cmdline));
    sprintf(cmdline,"wpa_cli -iwlan0 select_network %d", id);
    printf("%s\n", cmdline);
    execute(cmdline,buff);
    execute_result = !strncmp(buff,"OK",2);

    if(!execute_result){
        perror("setNetWorkPWD failed.\n");
        return FALSE;
    }

    return TRUE;
}

static bool save_wifi_config(const char* name, const char* pwd)
{
    FILE *fp;
    char body[256];
    int fd;
    fp = fopen("/data/cfg/wpa_supplicant.conf", "w");

    if (fp == NULL)
    {
        return -1;
    }

    snprintf(body, sizeof(body), WIFI_CONFIG_FORMAT, name, pwd);
    fputs(body, fp);
    fflush(fp);
    fd = fileno(fp);
    if (fd >= 0) {
        fsync(fd);
        printf("save wpa_supplicant.conf sucecees.\n");
    }
    fclose(fp);

    return 0;
}

static DBusMessage *chr_read_value(DBusConnection *conn, DBusMessage *msg,
							void *user_data)
{
	struct characteristic *chr = user_data;
	DBusMessage *reply;
	DBusMessageIter iter;
	const char *device;

	if (!dbus_message_iter_init(msg, &iter))
		return g_dbus_create_error(msg, DBUS_ERROR_INVALID_ARGS,
							"Invalid arguments");

	if (parse_options(&iter, &device))
		return g_dbus_create_error(msg, DBUS_ERROR_INVALID_ARGS,
							"Invalid arguments");

	reply = dbus_message_new_method_return(msg);
	if (!reply)
		return g_dbus_create_error(msg, DBUS_ERROR_NO_MEMORY,
							"No Memory");

	dbus_message_iter_init_append(reply, &iter);

	chr_read(chr, &iter);

	return reply;
}

static DBusMessage *chr_write_value(DBusConnection *conn, DBusMessage *msg,
							void *user_data)
{
	struct characteristic *chr = user_data;
	DBusMessageIter iter;
	const uint8_t *value;
	int len;
	const char *device;

	dbus_message_iter_init(msg, &iter);

	if (parse_value(&iter, &value, &len))
		return g_dbus_create_error(msg, DBUS_ERROR_INVALID_ARGS,
							"Invalid arguments");

	if (parse_options(&iter, &device))
		return g_dbus_create_error(msg, DBUS_ERROR_INVALID_ARGS,
							"Invalid arguments");

#ifdef DUEROS
    printf("chr_write_value, len: %d\n", len);
    for(int i = 0; i < len; i++)
        printf("%02x ", value[i]);
    printf("\n\n");

    if (!strcmp(DUEROS_CHARACTERISTIC_UUID, chr->uuid)) {
        dueros_socket_send((char *) value, len);
    }

#else
    if (!strcmp(SSID_UUID, chr->uuid)) {
        strcpy(wifi_ssid, value);
        printf("wifi ssid is  %s\n", wifi_ssid);
    }
    if (!strcmp(PASSWORD_UUID, chr->uuid)) {
        strcpy(wifi_password, value);
        printf("wifi pwd is  %s\n", wifi_password);
        #if 1
        wpa_cli_config_wifi();
        save_wifi_config(wifi_ssid, wifi_password);
        #else
        wpa_supplicant_config_wifi();
        #endif
    }
#endif

	return dbus_message_new_method_return(msg);
}

static DBusMessage *chr_start_notify(DBusConnection *conn, DBusMessage *msg,
							void *user_data)
{
	return g_dbus_create_error(msg, DBUS_ERROR_NOT_SUPPORTED,
							"Not Supported");
}

static DBusMessage *chr_stop_notify(DBusConnection *conn, DBusMessage *msg,
							void *user_data)
{
	return g_dbus_create_error(msg, DBUS_ERROR_NOT_SUPPORTED,
							"Not Supported");
}

static const GDBusMethodTable chr_methods[] = {
	{ GDBUS_ASYNC_METHOD("ReadValue", GDBUS_ARGS({ "options", "a{sv}" }),
					GDBUS_ARGS({ "value", "ay" }),
					chr_read_value) },
	{ GDBUS_ASYNC_METHOD("WriteValue", GDBUS_ARGS({ "value", "ay" },
						{ "options", "a{sv}" }),
					NULL, chr_write_value) },
	{ GDBUS_ASYNC_METHOD("StartNotify", NULL, NULL, chr_start_notify) },
	{ GDBUS_METHOD("StopNotify", NULL, NULL, chr_stop_notify) },
	{ }
};

static DBusMessage *desc_read_value(DBusConnection *conn, DBusMessage *msg,
							void *user_data)
{
	struct descriptor *desc = user_data;
	DBusMessage *reply;
	DBusMessageIter iter;
	const char *device;

	if (!dbus_message_iter_init(msg, &iter))
		return g_dbus_create_error(msg, DBUS_ERROR_INVALID_ARGS,
							"Invalid arguments");

	if (parse_options(&iter, &device))
		return g_dbus_create_error(msg, DBUS_ERROR_INVALID_ARGS,
							"Invalid arguments");

	reply = dbus_message_new_method_return(msg);
	if (!reply)
		return g_dbus_create_error(msg, DBUS_ERROR_NO_MEMORY,
							"No Memory");

	dbus_message_iter_init_append(reply, &iter);

	desc_read(desc, &iter);

	return reply;
}

static DBusMessage *desc_write_value(DBusConnection *conn, DBusMessage *msg,
							void *user_data)
{
	struct descriptor *desc = user_data;
	DBusMessageIter iter;
	const char *device;
	const uint8_t *value;
	int len;

	if (!dbus_message_iter_init(msg, &iter))
		return g_dbus_create_error(msg, DBUS_ERROR_INVALID_ARGS,
							"Invalid arguments");

	if (parse_value(&iter, &value, &len))
		return g_dbus_create_error(msg, DBUS_ERROR_INVALID_ARGS,
							"Invalid arguments");

	if (parse_options(&iter, &device))
		return g_dbus_create_error(msg, DBUS_ERROR_INVALID_ARGS,
							"Invalid arguments");

	desc_write(desc, value, len);

	return dbus_message_new_method_return(msg);
}

static const GDBusMethodTable desc_methods[] = {
	{ GDBUS_ASYNC_METHOD("ReadValue", GDBUS_ARGS({ "options", "a{sv}" }),
					GDBUS_ARGS({ "value", "ay" }),
					desc_read_value) },
	{ GDBUS_ASYNC_METHOD("WriteValue", GDBUS_ARGS({ "value", "ay" },
						{ "options", "a{sv}" }),
					NULL, desc_write_value) },
	{ }
};

static gboolean register_characteristic(const char *chr_uuid,
						const uint8_t *value, int vlen,
						const char **props,
						const char *desc_uuid,
						const char **desc_props,
						const char *service_path)
{
	struct characteristic *chr;
	struct descriptor *desc;
	static int id = 1;

	chr = g_new0(struct characteristic, 1);
	chr->uuid = g_strdup(chr_uuid);
	chr->value = g_memdup(value, vlen);
	chr->vlen = vlen;
	chr->props = props;
	chr->service = g_strdup(service_path);
	chr->path = g_strdup_printf("%s/characteristic%d", service_path, id++);

	if (!g_dbus_register_interface(connection, chr->path, GATT_CHR_IFACE,
					chr_methods, NULL, chr_properties,
					chr, chr_iface_destroy)) {
		printf("Couldn't register characteristic interface\n");
		chr_iface_destroy(chr);
		return FALSE;
	}

	if (!desc_uuid)
		return TRUE;

	desc = g_new0(struct descriptor, 1);
	desc->uuid = g_strdup(desc_uuid);
	desc->chr = chr;
	desc->props = desc_props;
	desc->path = g_strdup_printf("%s/descriptor%d", chr->path, id++);

	if (!g_dbus_register_interface(connection, desc->path,
					GATT_DESCRIPTOR_IFACE,
					desc_methods, NULL, desc_properties,
					desc, desc_iface_destroy)) {
		printf("Couldn't register descriptor interface\n");
		g_dbus_unregister_interface(connection, chr->path,
							GATT_CHR_IFACE);

		desc_iface_destroy(desc);
		return FALSE;
	}

#ifdef DUEROS
    if (strcmp(chr_uuid, DUEROS_CHARACTERISTIC_UUID) == 0) {
        printf("save dueros characteristic\n");
        dueros_chr = chr;
    }
#endif

	return TRUE;
}

static char *register_service(const char *uuid)
{
	static int id = 1;
	char *path;

	path = g_strdup_printf("/service%d", id++);
	if (!g_dbus_register_interface(connection, path, GATT_SERVICE_IFACE,
				NULL, NULL, service_properties,
				g_strdup(uuid), g_free)) {
		printf("Couldn't register service interface\n");
		g_free(path);
		return NULL;
	}

	return path;
}

#ifdef DUEROS
static void create_wifi_services(void)
{
	char *service_path;
	uint8_t level = 20;

	service_path = register_service(DUEROS_WIFI_SERVICES_UUID);
	if (!service_path)
		return;

	gboolean mSsidPassword = register_characteristic(DUEROS_CHARACTERISTIC_UUID,
						&level, sizeof(level),
						chr_props,
						CONFIG_UUID, /*NULL*/
						desc_props,
						service_path);

	/* Add Wifi Config Characteristic to Immediate Wifi Config Service */
	if (!mSsidPassword) {
		printf("Couldn't register wifi config characteristic (IAS)\n");
		g_dbus_unregister_interface(connection, service_path,
							GATT_SERVICE_IFACE);
		g_free(service_path);
		return;
	}

	services = g_slist_prepend(services, service_path);
	printf("Registered service: %s\n", service_path);
}
#else
static void create_wifi_services(void)
{
	char *service_path;
	uint8_t level = 20;

	service_path = register_service(WIFI_SERVICES_UUID);
	if (!service_path)
		return;

	gboolean mSecure = register_characteristic(SECURITY_UUID,
						&level, sizeof(level),
						chr_props,
						NULL,
						desc_props,
						service_path);
	gboolean mSsid = register_characteristic(SSID_UUID,
						&level, sizeof(level),
						chr_props,
						NULL,
						desc_props,
						service_path);
	gboolean mPassword = register_characteristic(PASSWORD_UUID,
						&level, sizeof(level),
						chr_props,
						NULL,
						desc_props,
						service_path);
	gboolean mConfigCharNotify = register_characteristic(CONFIG_NOTIFY_UUID,
						&level, sizeof(level),
						chr_props,
						CONFIG_UUID,
						desc_props,
						service_path);

	/* Add Wifi Config Characteristic to Immediate Wifi Config Service */
	if (!mSecure || !mSsid || !mPassword || !mConfigCharNotify) {
		printf("Couldn't register Wifi Config characteristic (IAS)\n");
		g_dbus_unregister_interface(connection, service_path,
							GATT_SERVICE_IFACE);
		g_free(service_path);
		return;
	}

	services = g_slist_prepend(services, service_path);
	printf("Registered service: %s\n", service_path);
}
#endif

static void send_advertise(){
        printf("send_advertise\n");
        char buff[256] = {0};
        char addr[6];
        char CMD_RA[256] = "hcitool -i hci0 cmd 0x08 0x0005 ";
        char temp[256];

        //creat random address
        int i;
        srand(time(NULL));
        for(i = 0; i < 6;i++)
                addr[i]=rand()&0xFF;

        addr[0] &= 0x3f;	/* Clear two most significant bits */
	    addr[0] |= 0x40;	/* Set second most significant bit */

        for(i = 0; i < 6;i++) {
              sprintf(temp,"%02x", addr[i]);
              strcat(CMD_RA, temp);
              strcat(CMD_RA, " ");
        }
        printf ("%s\n", CMD_RA);

        //LE Set Random Address Command
        execute(CMD_RA, buff);
        sleep(1);

        // LE Set Advertising Data Command
        execute(CMD_ADV, buff);
        sleep(1);

        // LE Set Advertise Enable Command
        execute(CMD_EN, buff);
}

static void register_app_reply(DBusMessage *reply, void *user_data)
{
    printf("register_app_reply\n");
	DBusError derr;

	dbus_error_init(&derr);
	dbus_set_error_from_message(&derr, reply);

	if (dbus_error_is_set(&derr))
		printf("RegisterApplication: %s\n", derr.message);
	else
		printf("RegisterApplication: OK\n");

	send_advertise();

	dbus_error_free(&derr);
}

static void register_app_setup(DBusMessageIter *iter, void *user_data)
{
	const char *path = "/";
	DBusMessageIter dict;

	dbus_message_iter_append_basic(iter, DBUS_TYPE_OBJECT_PATH, &path);

	dbus_message_iter_open_container(iter, DBUS_TYPE_ARRAY, "{sv}", &dict);

	/* TODO: Add options dictionary */

	dbus_message_iter_close_container(iter, &dict);
}

static void register_app(GDBusProxy *proxy)
{
	if (!g_dbus_proxy_method_call(proxy, "RegisterApplication",
					register_app_setup, register_app_reply,
					NULL, NULL)) {
		printf("Unable to call RegisterApplication\n");
		return;
	}
}

static void proxy_added_cb(GDBusProxy *proxy, void *user_data)
{
	const char *iface;

	iface = g_dbus_proxy_get_interface(proxy);

	if (g_strcmp0(iface, GATT_MGR_IFACE))
		return;

	register_app(proxy);
}

static gboolean signal_handler(GIOChannel *channel, GIOCondition cond,
							gpointer user_data)
{
	static bool __terminated = false;
	struct signalfd_siginfo si;
	ssize_t result;
	int fd;

	if (cond & (G_IO_NVAL | G_IO_ERR | G_IO_HUP))
		return FALSE;

	fd = g_io_channel_unix_get_fd(channel);

	result = read(fd, &si, sizeof(si));
	if (result != sizeof(si))
		return FALSE;

	switch (si.ssi_signo) {
	case SIGINT:
	case SIGTERM:
		if (!__terminated) {
			printf("Terminating\n");
			g_main_loop_quit(main_loop);
		}

		__terminated = true;
		break;
	}

	return TRUE;
}

static guint setup_signalfd(void)
{
	GIOChannel *channel;
	guint source;
	sigset_t mask;
	int fd;

	sigemptyset(&mask);
	sigaddset(&mask, SIGINT);
	sigaddset(&mask, SIGTERM);

	if (sigprocmask(SIG_BLOCK, &mask, NULL) < 0) {
		perror("Failed to set signal mask");
		return 0;
	}

	fd = signalfd(-1, &mask, 0);
	if (fd < 0) {
		perror("Failed to create signal descriptor");
		return 0;
	}

	channel = g_io_channel_unix_new(fd);

	g_io_channel_set_close_on_unref(channel, TRUE);
	g_io_channel_set_encoding(channel, NULL, NULL);
	g_io_channel_set_buffered(channel, FALSE);

	source = g_io_add_watch(channel,
				G_IO_IN | G_IO_HUP | G_IO_ERR | G_IO_NVAL,
				signal_handler, NULL);

	g_io_channel_unref(channel);

	return source;
}

int main(int argc, char *argv[])
{
	GDBusClient *client;
	guint signal;

	signal = setup_signalfd();
	if (signal == 0)
		return -errno;

	connection = g_dbus_setup_bus(DBUS_BUS_SYSTEM, NULL, NULL);

	main_loop = g_main_loop_new(NULL, FALSE);

	g_dbus_attach_object_manager(connection);

	printf("gatt-service unique name: %s\n",
				dbus_bus_get_unique_name(connection));

#ifdef DUEROS
    dueros_socket_thread_create();
#endif

	create_wifi_services();

	client = g_dbus_client_new(connection, "org.bluez", "/");

	g_dbus_client_set_proxy_handlers(client, proxy_added_cb, NULL, NULL,
									NULL);

	g_main_loop_run(main_loop);

#ifdef DUEROS
    dueros_socket_thread_delete();
#endif

	g_dbus_client_unref(client);

	g_source_remove(signal);

	g_slist_free_full(services, g_free);
	dbus_connection_unref(connection);

	return 0;
}
