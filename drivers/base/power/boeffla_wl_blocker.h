/*
 * Author: andip71, 01.09.2017
 *
 * Version 1.1.0
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#define BOEFFLA_WL_BLOCKER_VERSION	"1.1.0"

#define LIST_WL_DEFAULT				"RMNET_DFC;DIAG_WS;[timerfd];alarmtimer;qcom_rx_wakelock;" \
									"wlan;wlan_wow_wl;wlan_extscan_wl;netmgr_wl;NETLINK;" \
									"a600000.ssusb;984000.qcom,qup_uart;hal_bluetooth_lock;" \
									"IPA_WS;IPA_CLIENT_APPS_WAN_COAL_CONS;input5;" \
									"IPA_CLIENT_APPS_WAN_LOW_LAT_CONS;IPA_CLIENT_APPS_LAN_CONS;" \
									"spi0.0;spi1.0;rmnet_ipa%d;rmnet_ctl;RMNET_SHS;pmo_wow_wl;" \
									"IPA_CLIENT_APPS_WAN_CONS;wlan_pno_wl;wlan_deauth_rec_wl;" \
									"wlan_auth_req_wl;wlan_ap_assoc_lost_wl;event1;event2;eventpoll;" \
									"elliptic_wake_source;event5;bq2597x-standalone;wlan_roam_ho_wl;" \
									"wlan_fw_rsp_wakelock;CHG_PLCY_MAIN_WL;fpc_ttw_wl;CHG_PLCY_CP_WL;smp2p-sleepstate;18800000.qcom,icnss;884000.qcom,qup_uart;charge_pump_master"

#define LENGTH_LIST_WL				3048
#define LENGTH_LIST_WL_DEFAULT		(strlen(LIST_WL_DEFAULT) + 1)
#define LENGTH_LIST_WL_SEARCH		LENGTH_LIST_WL + LENGTH_LIST_WL_DEFAULT + 5
