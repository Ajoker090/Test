// ARF24_DEMODlg.cpp : 实现文件
//

#include "stdafx.h"
#include <stdlib.h>
#include <iostream>
#include <sstream>
#include <string>
#include <cstring>
#include <afx.h>

#include "ARF24_DEMO.h"
#include "ARF24_DEMODlg.h"
#include "SetTag.h"
#include "SetRouter.h"
#include "math.h"

#include "mysql_connection.h"
#include <cppconn/driver.h>
#include <cppconn/exception.h>
#include <cppconn/resultset.h>
#include <cppconn/statement.h>
#include <cppconn/sqlstring.h>
#include <cppconn/prepared_statement.h>

using namespace std;
using namespace sql;

#ifdef _DEBUG
#define new DEBUG_NEW
#endif

int		g_baud[] = {9600, 19200, 38400, 57600, 115200};
BYTE	g_CMD_TEST_BAUD_RATE[] = {0xFC, 0x00, 0x91, 0x07, 0x97, 0xA7, 0xD2};
BYTE	g_CMD_GET_PANID[] = {0xFC, 0x00, 0x91, 0x03, 0xA3, 0xB3, 0xE6};
BYTE	g_CMD_GET_SHORTADDR[] = {0xFC, 0x00, 0x91, 0x04, 0xC4, 0xD4, 0x29};
BYTE	g_CMD_GET_MACADDR[] = {0xFC, 0x00, 0x91, 0x08, 0xA8, 0xB8, 0xF5};
BYTE	g_CMD_GET_TYPE[] = {0xFC, 0x00, 0x91, 0x0B, 0xCB, 0xEB, 0x4E};
BYTE	g_CMD_SET_TO_COORDINATOR[] = {0xFC, 0x00, 0x91, 0x09, 0xA9, 0xC9, 0x08};
BYTE	g_CMD_SET_TO_ROUTER[] = {0xFC, 0x00, 0x91, 0x0A, 0xBA, 0xDA, 0x2B};
BYTE	g_CMD_GET_CHANNEL[] = {0xFC, 0x00, 0x91, 0x0D, 0x34, 0x2B, 0xF9};
BYTE	g_CMD_TEST_FIRMWARE[] = {0xFC, 0x00, 0x91, 0x0B, 0xCB, 0xEB, 0x4E};

BYTE	g_CMD_TEST_BAUD_RATE_DMATEK[] = {0xFC, 0x01, 0x02, 0x0A, 0x03, 0x04, 0x0A};

CRITICAL_SECTION    g_cs;

//串口读线程函数
DWORD ComReadThread(LPVOID lparam)
{	
	DWORD	actualReadLen=0;	//实际读取的字节数
	DWORD	willReadLen;	
	
	DWORD dwReadErrors;
	COMSTAT	cmState;
	
	CARF24_DEMODlg *pdlg;
	pdlg = (CARF24_DEMODlg *)lparam;

	pdlg->m_Receive_Data_Len = 0;
		
	// 清空缓冲，并检查串口是否打开。
	ASSERT(pdlg->m_hcom != INVALID_HANDLE_VALUE); 	
	//清空串口
	PurgeComm(pdlg->m_hcom, PURGE_RXCLEAR | PURGE_TXCLEAR );	
	SetCommMask (pdlg->m_hcom, EV_RXCHAR | EV_CTS | EV_DSR);
	try {
			sql::Driver *driver;
			sql::Connection *con;
			sql::Statement *stmt;
//			sql::ResultSet *res;
			sql::PreparedStatement *pstmt;

			/* Create a connection */
			driver = get_driver_instance();
			con = driver->connect("tcp://127.0.0.1:3306", "root", "lovelahm16");
			/* Connect to the MySQL test database */
			con->setSchema("test");

			stmt = con->createStatement();
			stmt->execute("DROP TABLE IF EXISTS test");
			stmt->execute("CREATE TABLE test(SpaceID INT, TagID char(40) BINARY,StartTime DATETIME, EndTime DATETIME)");
			delete stmt;
			/* '?' is the supported placeholder syntax */
			pstmt = con->prepareStatement("INSERT INTO test(SpaceID) VALUES (?)");
			for (int i = 1; i <= 6; i++) {
				pstmt->setInt(1, i);
				//pstmt->setString(2, "00124B00019A8FDB");
				//else if ( i == 2 ) pstmt->setString(2, "00124B00019A8F6F");
				//else pstmt->setString(2, " ");
				pstmt->executeUpdate();
			}
			delete pstmt;
			while(pdlg->m_hcom != NULL && pdlg->m_hcom != INVALID_HANDLE_VALUE){
				ClearCommError(pdlg->m_hcom,&dwReadErrors,&cmState);
				willReadLen = cmState.cbInQue ;
				if (willReadLen <= 0)
				{
					Sleep(10);
					continue;
				}			
				if(willReadLen + pdlg->m_Receive_Data_Len > MAX_RECEIVE_DATA_LEN)
					willReadLen = MAX_RECEIVE_DATA_LEN - pdlg->m_Receive_Data_Len;


				ReadFile(pdlg->m_hcom, pdlg->m_Receive_Data_Char + pdlg->m_Receive_Data_Len, willReadLen, &actualReadLen, 0);
				if (actualReadLen <= 0)
				{
					Sleep(10);
					continue;
				}
				pdlg->m_Receive_Data_Len += actualReadLen;

				EnterCriticalSection(&g_cs); 
				pdlg->ParseData(driver,con,stmt,pstmt);
				LeaveCriticalSection(&g_cs); 
		}		
	}
	catch (sql::SQLException &e) {
		cout << "# ERR: SQLException in " << __FILE__;
		cout << "(" << __FUNCTION__ << ") on line " << __LINE__ << endl;
		cout << "# ERR: " << e.what();
		cout << " (MySQL error code: " << e.getErrorCode();
		cout << ", SQLState: " << e.getSQLState() << " )" << endl;
	}
	cout << endl;
	return 0;
}

void CARF24_DEMODlg::ParseData(sql::Driver *driver, sql::Connection *con, sql::Statement *stmt, sql::PreparedStatement *pstmt)
{
	int start;
	bool isbreak = false;
	CString str, str_log;
	
		while(!isbreak){
			switch(m_lastcmd){
				case CMD_NULL:{
					/*
					ROUTER送出去的数据格式：                                            
					0xFF 0xFF：固定头，2Byte                                                    2
					Type：数据类型，1Byte     = AR_DATA_TYPE_TAG_SIGNALSTRENGTH                 1
					Len：数据长度，1Byte      ：8+8+1+1+1+1+1 = 21                              1
					DATA0---DATA7：Router的MAC地址数据，8 Byte                                  8
					DATA0---DATA7：Tag的MAC地址数据，8 Byte                                     8
					DATA0：信号强度，1 Byte                                                     1
					DATA0：温度整数，1 Byte                                                     1
					DATA0：温度小数，1 Byte                                                     1
					DATA0：湿度整数，1 Byte                                                     1
					DATA0：湿度小数，1 Byte                                                     1
					CheckSum：校验和，从头开始到数据结尾的所有数据的校验和。1Byte               1
					总长度：26                                                                   26
					*/
					if(m_Receive_Data_Len < 26)
						return;			//接收的数据长度不够，等待接收完成
					//查找开始标志字符
					start = FindChar(m_Receive_Data_Char, m_Receive_Data_Len, 0xFF, 0xFF);
					if(start < 0)
					{
						//没有开始标志字符，丢掉
						m_Receive_Data_Len = 0;
						return;
					}
					if(start + 26 > m_Receive_Data_Len)
					{
						//该封包没有接收完，等接受完在处理
						return;
					}
					if(m_Receive_Data_Char[start+2] != 0x01 || m_Receive_Data_Char[start+3] != 21)
					{
						//包头不对，丢掉
						m_Receive_Data_Len = m_Receive_Data_Len - start - 4;
						memcpy(m_Receive_Data_Char, m_Receive_Data_Char+start+4, m_Receive_Data_Len);
						break;
					}
					//判断校验和
					BYTE checksum = 0;
					for(int i=0; i<25; i++)
					{
						checksum += m_Receive_Data_Char[start+i];
					}
					if(checksum != m_Receive_Data_Char[start+25])
					{
						//校验和不正确，丢掉这些数据
						m_Receive_Data_Len = m_Receive_Data_Len - start - 26;
						memcpy(m_Receive_Data_Char, m_Receive_Data_Char+start+26, m_Receive_Data_Len);
						break;
					}
					//把tag和Router提取出来 from save file
					memcpy(m_current_router, m_Receive_Data_Char+start+4, ID_LEN);
					memcpy(m_current_tag, m_Receive_Data_Char+start+4+8, ID_LEN);
					m_current_single = (char)m_Receive_Data_Char[start+20];
					m_current_T_INT = m_Receive_Data_Char[start+21];
					m_current_T_DEC = m_Receive_Data_Char[start+22];
					m_current_H_INT = m_Receive_Data_Char[start+23];
					m_current_H_DEC = m_Receive_Data_Char[start+24];

					//寻找当前这个Tag是否已经保存过
					int index = -1;
					for(int i=0; i<MAX_RECEIVEINFO_COUNT; i++)
					{
						if(m_receiveinfo[i].ReceivData && memcmp(m_receiveinfo[i].TagId, m_current_tag, ID_LEN) == 0)
						{
							index = i;
							break;
						}
					}
					if(index == -1)
					{
						//这个tag没有收到过
						//查找第一个空的地方，来放该tag的信息
						for(int i=0; i<MAX_RECEIVEINFO_COUNT; i++)
						{
							if(!m_receiveinfo[i].ReceivData)
							{
								index = i;
								if ( index == 0 ) {
									pstmt = con->prepareStatement("UPDATE test SET StartTime = now(), TagID = ? where SpaceID = ?");
									pstmt ->setInt(2,index+1);
									pstmt ->setString(1, "00124B00019A8F6F");
									pstmt->executeUpdate();
								}
								else{
									pstmt = con->prepareStatement("UPDATE test SET StartTime = now(), TagID = ? where SpaceID = ?");
									pstmt ->setInt(2,index+1);
									pstmt ->setString(1, "00124B00019A8F8A");
									pstmt->executeUpdate();
								}
								delete pstmt;
								break;
							}
						}					
						memcpy(m_receiveinfo[index].TagId, m_current_tag, ID_LEN); //char_type
						m_receiveinfo[index].ReceivData = true;
						m_receiveinfo[index].FirstReceiveTime = GetTickCount(); 
						memcpy(m_receiveinfo[index].RouterId, m_current_router, ID_LEN);//char_type
						m_receiveinfo[index].Single = m_current_single;
						m_receiveinfo[index].T_INT = m_current_T_INT;  //Temperature
						m_receiveinfo[index].T_DEC = m_current_T_DEC;
						m_receiveinfo[index].H_INT = m_current_H_INT;  //Humidity
						m_receiveinfo[index].H_DEC = m_current_H_DEC;
				  
						/*Update the data*/
					//	IdToString(str, (char *)m_current_tag);
					//	string str2 = (char *)m_current_tag;

						/*Update the data*/
					//	char *c = new char [40];
					//	strcpy(c, reinterpret_cast<char*>(m_current_tag));
						if ( index == 0 ) {
							pstmt = con->prepareStatement("UPDATE test SET EndTime = now(), TagID = ? where SpaceID = ?");
							pstmt ->setInt(2,index+1);
							pstmt ->setString(1, "00124B00019A8F6F");
							pstmt->executeUpdate();
						}
						else{
							pstmt = con->prepareStatement("UPDATE test SET EndTime = now(), TagID = ? where SpaceID = ?");
							pstmt ->setInt(2,index+1);
							pstmt ->setString(1, "00124B00019A8F8A");
							pstmt->executeUpdate();
						}
						delete pstmt;
					//	delete c;
					//	delete con; 
					
					}
					else
					{
						//这个Tag已经收到过，判断是不是更近的Router 的ID
						if(m_current_single > m_receiveinfo[index].Single)
						{
							m_receiveinfo[index].Single = m_current_single;
							memcpy(m_receiveinfo[index].RouterId, m_current_router, ID_LEN);
						}
												/*Update the data*/
		//				stmt = con->createStatement();
		//				stmt ->execute("update test SET EndTime = now() where SpaceID =3");
		//				delete stmt;
						/*Update the data*/
		//				pstmt = con->prepareStatement("UPDATE test SET EndTimeTime = now() where SpaceID = ?");
		//				pstmt ->setInt(1,index+1);
		//				pstmt->executeUpdate();
		//				delete pstmt;
					}
					//查找当前接收到信号的Router是不是4个指定Router中的一个
					for(int i=0; i<MAX_ROUTER_COUNT; i++)
					{
						if(memcmp(m_saveinfo.router[i].ID, m_current_router, ID_LEN) == 0)
						{
							m_receiveinfo[index].SingleToRouter[i] = m_current_single;
						}
					}
					//处理完成，丢掉当前的这些数据
					m_Receive_Data_Len = m_Receive_Data_Len - start - 26;
					memcpy(m_Receive_Data_Char, m_Receive_Data_Char+start+26, m_Receive_Data_Len);
					//把获取到的数据保存到Log文件中
					CTime t = CTime::GetCurrentTime();
					str.Format(TEXT("   %02d:%02d:%02d  "), t.GetHour(), t.GetMinute(), t.GetSecond());
					str_log = str;
			
					IdToString(str, (char *)m_current_router);
					str_log += str + TEXT("  ");

					IdToString(str, (char *)m_current_tag);
					str_log += str + TEXT("  ");

					str.Format(TEXT("%03d"), m_current_single);
					str_log += str + TEXT("\r\n");

					DWORD write = str_log.GetLength();
					char s[128];
					wcstombs(s, str_log, str_log.GetLength());
					WriteFile(m_hfile_log, s, write, &write, NULL);
					
				//	pstmt = con->prepareStatement("UPDATE test SET Record = ? where SpaceID = ?");
				//	pstmt ->setInt(2,1);
				//	pstmt ->setString(1, s);
				//	pstmt->executeUpdate();
				//	delete pstmt;
				}
				break;	
			case CMD_TEST_BAUD:
				{
					if(m_Receive_Data_Len < 5)
						return;			//接收的数据长度不够，等待接收完成
					if(m_Receive_Data_Char[0] == 0x01 && m_Receive_Data_Char[1] == 0x02 &&
						m_Receive_Data_Char[2] == 0x03 && m_Receive_Data_Char[3] == 0x04 &&
						m_Receive_Data_Char[4] == 0x05)
					{
						m_test_baudrate_ok = true;
					}
					m_lastcmd = CMD_NULL;
					//丢掉处理过的数据
					m_Receive_Data_Len = m_Receive_Data_Len - 5;
					memcpy(m_Receive_Data_Char, m_Receive_Data_Char+5, m_Receive_Data_Len);
				}
				break;	
			case CMD_TEST_BAUD_DMATEK:
				{
					if(m_Receive_Data_Len < 3)
						return;			//接收的数据长度不够，等待接收完成
					if(m_Receive_Data_Char[0] == 0x03 && m_Receive_Data_Char[1] == 0x04 &&
						m_Receive_Data_Char[2] == 0x0A)
					{
						m_test_baudrate_ok = true;
					}
					m_lastcmd = CMD_NULL;
					//丢掉处理过的数据
					m_Receive_Data_Len = m_Receive_Data_Len - 3;
					memcpy(m_Receive_Data_Char, m_Receive_Data_Char+3, m_Receive_Data_Len);
				}
				break;	
			}
		}						
}

int CARF24_DEMODlg::FindChar(BYTE *str, int strlen, BYTE c1, BYTE c2)
{
	for(int i=0; i<strlen-1; i++)
	{
		if(str[i] == c1 && str[i+1] == c2)
			return i;
	}
	return -1;
}

// 用于应用程序“关于”菜单项的 CAboutDlg 对话框

class CAboutDlg : public CDialog
{
public:
	CAboutDlg();

// 对话框数据
	enum { IDD = IDD_ABOUTBOX };

	protected:
	virtual void DoDataExchange(CDataExchange* pDX);    // DDX/DDV 支持

// 实现
protected:
	DECLARE_MESSAGE_MAP()
};

CAboutDlg::CAboutDlg() : CDialog(CAboutDlg::IDD)
{
}

void CAboutDlg::DoDataExchange(CDataExchange* pDX)
{
	CDialog::DoDataExchange(pDX);
}

BEGIN_MESSAGE_MAP(CAboutDlg, CDialog)
END_MESSAGE_MAP()


// CARF24_DEMODlg 对话框




CARF24_DEMODlg::CARF24_DEMODlg(CWnd* pParent /*=NULL*/)
	: CDialog(CARF24_DEMODlg::IDD, pParent)
{
	m_hIcon = AfxGetApp()->LoadIcon(IDR_MAINFRAME);
}

void CARF24_DEMODlg::DoDataExchange(CDataExchange* pDX)
{
	CDialog::DoDataExchange(pDX);
	DDX_Control(pDX, IDC_BUTTON_CONNCET, m_button_connect);
	DDX_Control(pDX, IDC_PICTURE_CONNECT, m_picture_connect);
	DDX_Control(pDX, IDC_PICTURE_DISCONNECT, m_picture_disconnect);
	DDX_Control(pDX, IDC_BUTTON_SET_TAG, m_button_set_tag);
	DDX_Control(pDX, IDC_BUTTON_SET_ROUTER, m_button_set_router);
	DDX_Control(pDX, IDC_LIST_INFO, m_list_info);
	DDX_Control(pDX, IDC_STATIC_COM, m_static_com);
	DDX_Control(pDX, IDC_STATIC_BAUD, m_static_baud);
	DDX_Control(pDX, IDC_COMBO_COM, m_combo_com);
	DDX_Control(pDX, IDC_COMBO_BAUD, m_combo_baud);
	DDX_Control(pDX, IDC_BUTTON_AUTO_CONNECT, m_button_autoconnect);
	DDX_Control(pDX, IDC_BUTTON_CLEAN, m_button_clean);
}

BEGIN_MESSAGE_MAP(CARF24_DEMODlg, CDialog)
	ON_WM_SYSCOMMAND()
	ON_WM_PAINT()
	ON_WM_QUERYDRAGICON()
	//}}AFX_MSG_MAP
	ON_BN_CLICKED(IDC_BUTTON_SET_TAG, &CARF24_DEMODlg::OnBnClickedButtonSetTag)
	ON_BN_CLICKED(IDC_BUTTON_SET_ROUTER, &CARF24_DEMODlg::OnBnClickedButtonSetRouter)
	ON_BN_CLICKED(IDC_BUTTON_CONNCET, &CARF24_DEMODlg::OnBnClickedButtonConncet)
	ON_BN_CLICKED(IDC_BUTTON_AUTO_CONNECT, &CARF24_DEMODlg::OnBnClickedButtonAutoConnect)
	ON_BN_CLICKED(IDC_BUTTON_CLEAN, &CARF24_DEMODlg::OnBnClickedButtonClean)
	ON_WM_TIMER()
END_MESSAGE_MAP()


// CARF24_DEMODlg 消息处理程序

BOOL CARF24_DEMODlg::OnInitDialog()
{
	CDialog::OnInitDialog();

	// 将“关于...”菜单项添加到系统菜单中。

	// IDM_ABOUTBOX 必须在系统命令范围内。
	ASSERT((IDM_ABOUTBOX & 0xFFF0) == IDM_ABOUTBOX);
	ASSERT(IDM_ABOUTBOX < 0xF000);

	CMenu* pSysMenu = GetSystemMenu(FALSE);
	if (pSysMenu != NULL)
	{
		CString strAboutMenu;
		strAboutMenu.LoadString(IDS_ABOUTBOX);
		if (!strAboutMenu.IsEmpty())
		{
			pSysMenu->AppendMenu(MF_SEPARATOR);
			pSysMenu->AppendMenu(MF_STRING, IDM_ABOUTBOX, strAboutMenu);
		}
	}

	// 设置此对话框的图标。当应用程序主窗口不是对话框时，框架将自动
	//  执行此操作
	SetIcon(m_hIcon, TRUE);			// 设置大图标
	SetIcon(m_hIcon, FALSE);		// 设置小图标

	// TODO: 在此添加额外的初始化代码

	MoveWindow(0, 0, APP_WIDTH, APP_HEIGHT);

	m_button_connect.MoveWindow(BUTTON_CONNECT_LEFT, BUTTON_CONNECT_TOP, BUTTON_CONNECT_WIDTH, BUTTON_CONNECT_HEIGHT);
	m_picture_connect.MoveWindow(PICTURE_CONNECT_LEFT, PICTURE_CONNECT_TOP, PICTURE_CONNECT_WIDTH, PICTURE_CONNECT_HEIGHT);
	m_picture_disconnect.MoveWindow(PICTURE_DISCONNECT_LEFT, PICTURE_DISCONNECT_TOP, PICTURE_DISCONNECT_WIDTH, PICTURE_DISCONNECT_HEIGHT);
	m_button_set_tag.MoveWindow(BUTTON_SETTAG_LEFT, BUTTON_SETTAG_TOP, BUTTON_SETTAG_WIDTH, BUTTON_SETTAG_HEIGHT);
	m_button_set_router.MoveWindow(BUTTON_SETROUTER_LEFT, BUTTON_SETROUTER_TOP, BUTTON_SETROUTER_WIDTH, BUTTON_SETROUTER_HEIGHT);
	m_static_baud.MoveWindow(STATIC_BAUD_LEFT, STATIC_BAUD_TOP, STATIC_BAUD_WIDTH, STATIC_BAUD_HEIGHT);
	m_static_com.MoveWindow(STATIC_COM_LEFT, STATIC_COM_TOP, STATIC_COM_WIDTH, STATIC_COM_HEIGHT);
	m_combo_com.MoveWindow(COMBO_COM_LEFT, COMBO_COM_TOP, COMBO_COM_WIDTH, COMBO_COM_HEIGHT);
	m_combo_baud.MoveWindow(COMBO_BAUD_LEFT, COMBO_BAUD_TOP, COMBO_BAUD_WIDTH, COMBO_BAUD_HEIGHT);
	m_button_autoconnect.MoveWindow(BUTTON_AUTOCONNECT_LEFT, BUTTON_AUTOCONNECT_TOP, BUTTON_AUTOCONNECT_WIDTH, BUTTON_AUTOCONNECT_HEIGHT);
	m_button_clean.MoveWindow(BUTTON_CLEAN_LEFT, BUTTON_CLEAN_TOP, BUTTON_CLEAN_WIDTH, BUTTON_CLEAN_HEIGHT);


	DWORD dwStyle = m_list_info.GetExtendedStyle();
	m_list_info.SetExtendedStyle(dwStyle | LVS_EX_GRIDLINES | LVS_EX_FULLROWSELECT | LVS_EX_HEADERDRAGDROP);
	m_list_info.InsertColumn(1,TEXT("TAG ID"), LVCFMT_CENTER, 120);
	m_list_info.InsertColumn(2,TEXT("TAG Name"), LVCFMT_CENTER, 100);
	m_list_info.InsertColumn(3,TEXT("Router ID"), LVCFMT_CENTER, 120);
	m_list_info.InsertColumn(4,TEXT("Router Name"), LVCFMT_CENTER, 100);	
	m_list_info.InsertColumn(5,TEXT("Signal"), LVCFMT_CENTER, 60);
	m_list_info.InsertColumn(6,TEXT("Temperature"), LVCFMT_CENTER, 100);
	m_list_info.InsertColumn(7,TEXT("Humidity"), LVCFMT_CENTER, 100);
	m_list_info.InsertColumn(8,TEXT("Time"), LVCFMT_CENTER, 120);

	m_combo_com.SetCurSel(0);
	m_combo_baud.SetCurSel(0);
	
	m_list_info.MoveWindow(LIST_INFO_LEFT, LIST_INFO_TOP, LIST_INFO_WIDTH, LIST_INFO_HEIGHT);

	InitializeCriticalSection(&g_cs); 
	m_bconnect = false;
	m_lastcmd = CMD_NULL;

	m_hdc = ::GetDC(this->GetSafeHwnd());
	m_mdc = CreateCompatibleDC(m_hdc);
	m_router_dc = CreateCompatibleDC(m_hdc);
	m_mbmp = CreateCompatibleBitmap(m_hdc, DRAW_WIDTH, DRAW_HEIGHT);
	m_routerbmp = CreateCompatibleBitmap(m_hdc, BMP_ROUTER_WIDTH, BMP_ROUTER_HEIGHT);
	HDC ndc = CreateCompatibleDC(m_hdc);
	CBitmap bmp;
	bmp.LoadBitmapW(IDB_BITMAP_ROUTER);
	SelectObject(ndc, bmp);
	SelectObject(m_router_dc, m_routerbmp);
	SelectObject(m_mdc, m_mbmp);
	BitBlt(m_router_dc, 0, 0, BMP_ROUTER_WIDTH, BMP_ROUTER_HEIGHT, ndc, 0, 0, SRCCOPY);

	m_Brush_white = ::CreateSolidBrush(RGB(255, 255, 255));
	m_Brush_Blue = ::CreateSolidBrush(RGB(0, 0, 255));

	GetTagRouterInfo();
	
	srand(GetTickCount());

	m_current_listitem_count = 0;

	return TRUE;  // 除非将焦点设置到控件，否则返回 TRUE
}

void CARF24_DEMODlg::OnSysCommand(UINT nID, LPARAM lParam)
{
	if ((nID & 0xFFF0) == IDM_ABOUTBOX)
	{
		CAboutDlg dlgAbout;
		dlgAbout.DoModal();
	}
	else
	{
		CDialog::OnSysCommand(nID, lParam);
	}
}

// 如果向对话框添加最小化按钮，则需要下面的代码
//  来绘制该图标。对于使用文档/视图模型的 MFC 应用程序，
//  这将由框架自动完成。

void CARF24_DEMODlg::OnPaint()
{
	if (IsIconic())
	{
		CPaintDC dc(this); // 用于绘制的设备上下文

		SendMessage(WM_ICONERASEBKGND, reinterpret_cast<WPARAM>(dc.GetSafeHdc()), 0);

		// 使图标在工作矩形中居中
		int cxIcon = GetSystemMetrics(SM_CXICON);
		int cyIcon = GetSystemMetrics(SM_CYICON);
		CRect rect;
		GetClientRect(&rect);
		int x = (rect.Width() - cxIcon + 1) / 2;
		int y = (rect.Height() - cyIcon + 1) / 2;

		// 绘制图标
		dc.DrawIcon(x, y, m_hIcon);
	}
	else
	{
		CDialog::OnPaint();

		Draw();
	}
}

//当用户拖动最小化窗口时系统调用此函数取得光标显示。
//
HCURSOR CARF24_DEMODlg::OnQueryDragIcon()
{
	return static_cast<HCURSOR>(m_hIcon);
}


void CARF24_DEMODlg::OnBnClickedButtonSetTag()
{
	// TODO: Add your control notification handler code here
	SetTag cst;
	cst.DoModal();
}

void CARF24_DEMODlg::OnBnClickedButtonSetRouter()
{
	// TODO: Add your control notification handler code here
	CSetRouter sr;
	sr.DoModal();
}

VOID CARF24_DEMODlg::GetModulePath(LPTSTR path, LPCTSTR module)
{
	TCHAR* s;
	HANDLE Handle = NULL;
	if(module)
		Handle = GetModuleHandle(module);
	GetModuleFileName((HMODULE)Handle, path, MAX_PATH);
	s = _tcsrchr(path, '\\');
	if(s) s[1] = 0;
}

BOOL CARF24_DEMODlg::GetTagRouterInfo()
{
	WCHAR szPath[MAX_PATH] = {0};
	GetModulePath(szPath, NULL);
	CString filepath;
	filepath = szPath;
	filepath = filepath + SAVE_FILE_NAME;
	m_hfile = CreateFile(filepath,GENERIC_WRITE|GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,NULL);   
  	if(m_hfile == INVALID_HANDLE_VALUE)   
	{   
		//文件不存在，创建文件
		m_hfile = CreateFile(filepath,GENERIC_WRITE|GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL,NULL);   
  		if(m_hfile == INVALID_HANDLE_VALUE)   
		{   
			//创建文件失败
			MessageBox(TEXT("Failed to create file !"));
			return FALSE;  
		}
		memset(&m_saveinfo, 0, sizeof(m_saveinfo));
		DWORD write;
		WriteFile(m_hfile, &m_saveinfo, sizeof(m_saveinfo), &write, NULL);
	} 
	else
	{
		//文件存在，读取数据
		DWORD Read;
		ReadFile(m_hfile, &m_saveinfo, sizeof(m_saveinfo), &Read, NULL);		
	}
	CloseHandle(m_hfile);
	
	
	memset(m_receiveinfo, 0, sizeof(m_receiveinfo));
	m_current_receive_count = 0;
	
	return TRUE;


}

void CARF24_DEMODlg::OpenLogFile()
{
	WCHAR szPath[MAX_PATH] = {0};
	GetModulePath(szPath, NULL);
	CString filepath;
	filepath = szPath;
	filepath = filepath + LOG_FILE_NAME;
	m_hfile_log = CreateFile(filepath,GENERIC_WRITE|GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,NULL);   
  	if(m_hfile_log == INVALID_HANDLE_VALUE)   
	{   		
		//创建文件失败
		MessageBox(TEXT("Failed to create Log file !"));
		return ;  		
	} 
}

bool CARF24_DEMODlg::OpenCom(int com)
{
	CString str;
	str.Format(TEXT("COM%d"), com);
	return OpenCom(str);
}

bool CARF24_DEMODlg::OpenCom(CString str_com)
{	
	str_com = TEXT("\\\\.\\") + str_com;
	m_hcom = CreateFile(str_com, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
	///配置串口
	if(m_hcom != INVALID_HANDLE_VALUE && m_hcom != NULL)
	{		
		DCB  dcb;    
		dcb.DCBlength = sizeof(DCB); 
		// 默认串口参数
		GetCommState(m_hcom, &dcb);

		dcb.BaudRate = CBR_115200;					// 设置波特率 
		dcb.fBinary = TRUE;						// 设置二进制模式，此处必须设置TRUE
		dcb.fParity = TRUE;						// 支持奇偶校验 
		dcb.ByteSize = 8;						// 数据位,范围:4-8 
		dcb.Parity = NOPARITY;					// 校验模式
		dcb.StopBits = ONESTOPBIT;				// 停止位 0,1,2 = 1, 1.5, 2

		dcb.fOutxCtsFlow = FALSE;				// No CTS output flow control 
		dcb.fOutxDsrFlow = FALSE;				// No DSR output flow control 
		dcb.fDtrControl = DTR_CONTROL_ENABLE; 
		// DTR flow control type 
		dcb.fDsrSensitivity = FALSE;			// DSR sensitivity 
		dcb.fTXContinueOnXoff = TRUE;			// XOFF continues Tx 
		dcb.fOutX = FALSE;					// No XON/XOFF out flow control 
		dcb.fInX = FALSE;						// No XON/XOFF in flow control 
		dcb.fErrorChar = FALSE;				// Disable error replacement 
		dcb.fNull = FALSE;					// Disable null stripping 
		dcb.fRtsControl = RTS_CONTROL_ENABLE; 
		// RTS flow control 
		dcb.fAbortOnError = FALSE;			// 当串口发生错误，并不终止串口读写


		if (!SetCommState(m_hcom, &dcb))
		{
			///L"配置串口失败";			
			return false;
		}
		////配置超时值
		COMMTIMEOUTS  cto;
		GetCommTimeouts(m_hcom, &cto);
		cto.ReadIntervalTimeout = MAXDWORD;  
		cto.ReadTotalTimeoutMultiplier = 10;  
		cto.ReadTotalTimeoutConstant = 10;    
		cto.WriteTotalTimeoutMultiplier = 50;  
		cto.WriteTotalTimeoutConstant = 100;    
		if (!SetCommTimeouts(m_hcom, &cto))
		{
			///L"不能设置超时参数";		
			return false;
		}	

		//指定端口监测的事件集
		SetCommMask (m_hcom, EV_RXCHAR);
		
		//分配设备缓冲区
	//	SetupComm(m_hcom,8192,8192);

		//初始化缓冲区中的信息
		PurgeComm(m_hcom,PURGE_TXCLEAR|PURGE_RXCLEAR);
	}
	else
	{		
		return false;
	}
	return true;
}


bool CARF24_DEMODlg::SetBaudRate(int baudrate)
{
	if(m_hcom != INVALID_HANDLE_VALUE && m_hcom != NULL)
	{		
		DCB  dcb;    
		dcb.DCBlength = sizeof(DCB); 
		// 默认串口参数
		GetCommState(m_hcom, &dcb);

		dcb.BaudRate = baudrate;

		if (!SetCommState(m_hcom, &dcb))
		{
			///L"配置串口失败";			
			return false;
		}
		return true;
	}
	return false;
}


void CARF24_DEMODlg::OnBnClickedButtonConncet()
{
	// TODO: Add your control notification handler code here
	if(m_bconnect)
	{
		m_button_connect.SetWindowTextW(TEXT("Connect Module"));
		m_button_autoconnect.SetWindowTextW(TEXT("Auto Connect"));
		m_picture_connect.ShowWindow(SW_HIDE);
		m_picture_disconnect.ShowWindow(SW_SHOW);
		KillTimer(1);
		CloseHandle(m_hcom);
		m_bconnect = false;
		CloseHandle(m_hfile_log);
	}
	else
	{
		GetTagRouterInfo();
		CString str_com;
		m_combo_com.GetWindowTextW(str_com);
		if(!OpenCom(str_com))
		{
			MessageBox(TEXT("Open ") + str_com + TEXT(" Fail !"));
			return;
		}
		int baud;
		baud = m_combo_baud.GetCurSel();
		SetBaudRate(g_baud[baud]);

		OpenLogFile();

		HANDLE m_hThread = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)ComReadThread, this, 0, NULL);
		CloseHandle(m_hThread);

		m_picture_connect.ShowWindow(SW_SHOW);
		m_picture_disconnect.ShowWindow(SW_HIDE);

		m_button_connect.SetWindowTextW(TEXT("DisConnect"));
		m_button_autoconnect.SetWindowTextW(TEXT("DisConnect"));

		SetTimer(1, 1000, NULL);
		OnBnClickedButtonClean();

		m_bconnect = true;
	}
}

void CARF24_DEMODlg::OnBnClickedButtonAutoConnect()
{
	// TODO: Add your control notification handler code here
	if(m_bconnect)
	{
		m_button_connect.SetWindowTextW(TEXT("Connect Module"));
		m_button_autoconnect.SetWindowTextW(TEXT("Auto Connect"));
		m_picture_connect.ShowWindow(SW_HIDE);
		m_picture_disconnect.ShowWindow(SW_SHOW);
		KillTimer(1);
		CloseHandle(m_hcom);
		m_bconnect = false;	
		CloseHandle(m_hfile_log);
	}
	else
	{
		GetTagRouterInfo();

		for(int m_com_index = 1; m_com_index<100; m_com_index++)
		{
			if(!OpenCom(m_com_index))
			{
				continue;
			}	
			HANDLE m_hThread = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)ComReadThread, this, 0, NULL);
			CloseHandle(m_hThread);
			for(int i=0; i<5; i++)
			{
				if(!SetBaudRate(g_baud[i]))
				{
					CString str;
					str.Format(TEXT("%d"), g_baud[i]);
					str = TEXT("Set Baud Rate:")+str+TEXT(" Fail!");
					MessageBox(str, NULL, MB_OK|MB_ICONHAND );
					return;
				}
				m_test_baudrate_ok = false;
				m_Receive_Data_Len = 0;
				m_lastcmd = CMD_TEST_BAUD;
				DWORD write;
				WriteFile(m_hcom, g_CMD_TEST_BAUD_RATE, sizeof(g_CMD_TEST_BAUD_RATE), &write, NULL);
				Sleep(100);
				if(m_test_baudrate_ok)
				{
					OpenLogFile();

					CString str;
					str.Format(TEXT("COM%d"), m_com_index);				
					m_combo_com.SetWindowTextW(str);

					m_combo_baud.SetCurSel(i);
					
					m_picture_connect.ShowWindow(SW_SHOW);
					m_picture_disconnect.ShowWindow(SW_HIDE);

					m_button_connect.SetWindowTextW(TEXT("DisConnect"));
					m_button_autoconnect.SetWindowTextW(TEXT("DisConnect"));

					SetTimer(1, 1000, NULL);
					OnBnClickedButtonClean();
					
					m_bconnect = true;
					return;
				}
				else
				{
					m_test_baudrate_ok = false;
					m_Receive_Data_Len = 0;
					m_lastcmd = CMD_TEST_BAUD_DMATEK;					
					WriteFile(m_hcom, g_CMD_TEST_BAUD_RATE_DMATEK, sizeof(g_CMD_TEST_BAUD_RATE_DMATEK), &write, NULL);
					Sleep(100);
					if(m_test_baudrate_ok)
					{
						OpenLogFile();

						CString str;
						str.Format(TEXT("COM%d"), m_com_index);				
						m_combo_com.SetWindowTextW(str);

						m_combo_baud.SetCurSel(i);
						
						m_picture_connect.ShowWindow(SW_SHOW);
						m_picture_disconnect.ShowWindow(SW_HIDE);

						m_button_connect.SetWindowTextW(TEXT("DisConnect"));
						m_button_autoconnect.SetWindowTextW(TEXT("DisConnect"));

						SetTimer(1, 1000, NULL);
						OnBnClickedButtonClean();
						
						m_bconnect = true;
						return;
					}
				}
			}
			CloseHandle(m_hcom);
			Sleep(10);
			
		}
		MessageBox(TEXT("Connect Zigbee Module Fail!"), NULL, MB_OK|MB_ICONEXCLAMATION );
		CloseHandle(m_hcom);
	}
}

void CARF24_DEMODlg::OnBnClickedButtonClean()
{
	// TODO: Add your control notification handler code here
	while(m_list_info.GetItemCount())
		m_list_info.DeleteItem(0);

	m_current_listitem_count = 0;
	Draw();
}

void CARF24_DEMODlg::OnTimer(UINT_PTR nIDEvent)
{
	// TODO: Add your message handler code here and/or call default
	EnterCriticalSection(&g_cs); 
	if(nIDEvent == 1)
	{
		int index_list;
		bool updatadraw = false;
		for(int i=0; i<MAX_RECEIVEINFO_COUNT; i++)
		{
			if(!m_receiveinfo[i].ReceivData)
				continue;
			if(GetTickCount() - m_receiveinfo[i].FirstReceiveTime < 1000)
				continue;

			index_list = FindTagIdInList(m_receiveinfo[i].TagId);
			if(index_list < 0)
			{
				//添加行			
				memcpy(m_listshowinfo[m_current_listitem_count].TagId, m_receiveinfo[i].TagId, ID_LEN);
				m_listshowinfo[m_current_listitem_count].index = m_current_listitem_count;
				index_list = m_current_listitem_count++;
				
				//添加tag ID
				CString str;
				IdToString(str, m_receiveinfo[i].TagId);
				m_list_info.InsertItem(index_list, str);
				
				//添加tag Name
				int index = FindTagIdInSave(m_receiveinfo[i].TagId);
				if(index != -1)
				{
					str = m_saveinfo.tag[index].Name;
					m_list_info.SetItemText(index_list, 1, str);
				}
			}
			else
			{
				
			}
			//更改行信息
			//修改Router ID
			CString str;
			IdToString(str, m_receiveinfo[i].RouterId);
			m_list_info.SetItemText(index_list, 2, str);

			//修改Router Name
			//添加tag Name
			int index = FindRouterIdInSave(m_receiveinfo[i].RouterId);
			if(index != -1)
			{
				str = m_saveinfo.router[index].Name;
				m_list_info.SetItemText(index_list, 3, str);
			}
			else
			{
				m_list_info.SetItemText(index_list, 3, TEXT(""));
			}
			
			//修改信号
			str.Format(TEXT("%d"), m_receiveinfo[i].Single);
			m_list_info.SetItemText(index_list, 4, str);

			//修改温度
			str.Format(TEXT("%d.%d"), m_receiveinfo[i].T_INT, m_receiveinfo[i].T_DEC);
			m_list_info.SetItemText(index_list, 5, str);

			//修改湿度
			str.Format(TEXT("%d.%d"), m_receiveinfo[i].H_INT, m_receiveinfo[i].H_DEC);
			m_list_info.SetItemText(index_list, 6, str);

			CTime t = CTime::GetCurrentTime();
			str.Format(TEXT("%02d:%02d:%02d"), t.GetHour(), t.GetMinute(), t.GetSecond());
			m_list_info.SetItemText(index_list, 7, str);
			
			memcpy(&m_listshowinfo[index_list].reciveinfo, &m_receiveinfo[i], sizeof(ReceiveInfo));
			
			//把定位的信息保存到文件
			CString str_log;
			str.Format(TEXT("*  %02d:%02d:%02d  "), t.GetHour(), t.GetMinute(), t.GetSecond());
			str_log = str;

			IdToString(str, (char *)m_receiveinfo[i].RouterId);
			str_log += str + TEXT("  ");

			IdToString(str, (char *)m_receiveinfo[i].TagId);
			str_log += str + TEXT("  ");

			str.Format(TEXT("%03d"), m_receiveinfo[i].Single);
			str_log += str + TEXT("\r\n");

			DWORD write = str_log.GetLength();
			char s[128];
			wcstombs(s, str_log, str_log.GetLength());
			WriteFile(m_hfile_log, s, write, &write, NULL);

			//处理完，清除标志位，
			memset(&m_receiveinfo[i], 0, sizeof(ReceiveInfo));			
			updatadraw = true;
		}
		if(updatadraw)
			Draw();
	}
	LeaveCriticalSection(&g_cs); 	

	CDialog::OnTimer(nIDEvent);
}

int CARF24_DEMODlg::FindTagIdInList(char *id)
{
	for(int i=0; i<m_current_listitem_count; i++)
	{
		if(memcmp(m_listshowinfo[i].TagId, id, ID_LEN) == 0)
			return i;
	}
	return -1;
}

int CARF24_DEMODlg::FindTagIdInSave(char *id)
{
	for(int i=0; i<MAX_TAG_COUNT; i++)
	{
		if(memcmp(m_saveinfo.tag[i].ID, id, ID_LEN) == 0)
			return i;
	}
	return -1;
}

int CARF24_DEMODlg::FindRouterIdInSave(char *id)
{
	for(int i=0; i<MAX_ROUTER_COUNT; i++)
	{
		if(memcmp(m_saveinfo.router[i].ID, id, ID_LEN) == 0)
			return i;
	}
	return -1;
}

//void CARF24_DEMODlg::IdToChar(char &c ,char *id){
	
//}
void CARF24_DEMODlg::IdToString(CString &str, char *id)
{
	CString s;
	str = TEXT("");
	for(int i=0; i<8; i++)
	{
		s.Format(TEXT("%02X"), (BYTE)id[i]);
		str += s;
	}
}

void CARF24_DEMODlg::Draw()
{
	//首先画框
	int x, y;

	RECT r;
	r.left = 0;
	r.top = 0;
	r.right = DRAW_WIDTH;
	r.bottom = DRAW_HEIGHT;

	CRect cr;

	::FillRect(m_mdc, &r, m_Brush_white);
	::MoveToEx(m_mdc, 0, 0, NULL);
	::LineTo(m_mdc, DRAW_WIDTH-1, 0);
	::LineTo(m_mdc, DRAW_WIDTH-1, DRAW_HEIGHT-1);
	::LineTo(m_mdc, 0, DRAW_HEIGHT-1);
	::LineTo(m_mdc, 0, 0);

	::MoveToEx(m_mdc, 0, DRAW_HEIGHT/2, NULL);
	::LineTo(m_mdc, DRAW_WIDTH/2*3/4, DRAW_HEIGHT/2);

	::MoveToEx(m_mdc, DRAW_WIDTH, DRAW_HEIGHT/2, NULL);
	::LineTo(m_mdc, DRAW_WIDTH - DRAW_WIDTH/2*3/4, DRAW_HEIGHT/2);

	::MoveToEx(m_mdc, DRAW_WIDTH/2, 0, NULL);
	::LineTo(m_mdc, DRAW_WIDTH/2, DRAW_HEIGHT/2*3/4);

	::MoveToEx(m_mdc, DRAW_WIDTH/2, DRAW_HEIGHT, NULL);
	::LineTo(m_mdc, DRAW_WIDTH/2, DRAW_HEIGHT - DRAW_HEIGHT/2*3/4);
	
	//画Router
	BitBlt(m_mdc, 1, 1, BMP_ROUTER_WIDTH, BMP_ROUTER_HEIGHT, m_router_dc, 0, 0, SRCCOPY);
	BitBlt(m_mdc, DRAW_WIDTH-1-BMP_ROUTER_WIDTH, 1, BMP_ROUTER_WIDTH, BMP_ROUTER_HEIGHT, m_router_dc, 0, 0, SRCCOPY);
	BitBlt(m_mdc, DRAW_WIDTH-1-BMP_ROUTER_WIDTH, DRAW_HEIGHT-1-BMP_ROUTER_HEIGHT, BMP_ROUTER_WIDTH, BMP_ROUTER_HEIGHT, m_router_dc, 0, 0, SRCCOPY);
	BitBlt(m_mdc, 1, DRAW_HEIGHT-1-BMP_ROUTER_HEIGHT, BMP_ROUTER_WIDTH, BMP_ROUTER_HEIGHT, m_router_dc, 0, 0, SRCCOPY);

	//给Router写上Name
	CString str;	
	SetTextColor(m_mdc, RGB(255, 0, 0));
	SetBkMode(m_mdc, TRANSPARENT);

	str = m_saveinfo.router[0].Name;
	if(str.IsEmpty())
		IdToString(str, m_saveinfo.router[0].ID);
	cr.SetRect(1, 1, BMP_ROUTER_WIDTH+1, 1+BMP_ROUTER_HEIGHT*3/4);
	::DrawText(m_mdc, str, -1, &cr, DT_CENTER|DT_BOTTOM|DT_SINGLELINE);

	str = m_saveinfo.router[1].Name;
	if(str.IsEmpty())
		IdToString(str, m_saveinfo.router[1].ID);
	cr.SetRect(DRAW_WIDTH-1-BMP_ROUTER_WIDTH, 1, DRAW_WIDTH-1, 1+BMP_ROUTER_HEIGHT*3/4);
	::DrawText(m_mdc, str, -1, &cr, DT_CENTER|DT_BOTTOM|DT_SINGLELINE);

	str = m_saveinfo.router[2].Name;
	if(str.IsEmpty())
		IdToString(str, m_saveinfo.router[2].ID);
	cr.SetRect(DRAW_WIDTH-1-BMP_ROUTER_WIDTH, DRAW_HEIGHT-1-BMP_ROUTER_HEIGHT, DRAW_WIDTH-1, DRAW_HEIGHT-1-BMP_ROUTER_HEIGHT/4);
	::DrawText(m_mdc, str, -1, &cr, DT_CENTER|DT_BOTTOM|DT_SINGLELINE);

	str = m_saveinfo.router[3].Name;
	if(str.IsEmpty())
		IdToString(str, m_saveinfo.router[3].ID);
	cr.SetRect(1, DRAW_HEIGHT-1-BMP_ROUTER_HEIGHT, BMP_ROUTER_WIDTH, DRAW_HEIGHT-1-BMP_ROUTER_HEIGHT/4);
	::DrawText(m_mdc, str, -1, &cr, DT_CENTER|DT_BOTTOM|DT_SINGLELINE);

	//画Tag点
	int router_index, tag_index;
	for(int i=0; i<m_current_listitem_count; i++)
	{
		if(GetTickCount() - m_listshowinfo[i].reciveinfo.FirstReceiveTime > 10*1000)
			continue;	//长时间没有更新数据的，不画
		
		//判断当前这个Tag，靠近的Router，是不是在保存的列表中，在，画图，不在，不画图
		router_index = FindRouterIdInSave(m_listshowinfo[i].reciveinfo.RouterId);
		if(router_index == -1)
		{
			//不在，不画图
			continue;
		}
		else
		{
			SelectObject(m_mdc, m_Brush_Blue);
			//在，画图
			//判断Tag是靠近哪一个Router
			switch(router_index)
			{
			case 0:		//左上角
				{
					if(m_listshowinfo[i].LastDrawSingle == m_listshowinfo[i].reciveinfo.Single)
					{
						//信号强度相同，不更新图标位置，使用上次的点坐标
						x = m_listshowinfo[i].LastDrawPoint.x;
						y = m_listshowinfo[i].LastDrawPoint.y;
					}
					else
					{					
						//随机产生一个坐标
						x = rand()%(m_listshowinfo[i].reciveinfo.Single);
						y = (int)sqrt((double)(m_listshowinfo[i].reciveinfo.Single * m_listshowinfo[i].reciveinfo.Single - x*x));
						
						x += BMP_ROUTER_WIDTH;
						y += BMP_ROUTER_HEIGHT;
					}
					Ellipse(m_mdc, x-DRAW_RADIUS, y-DRAW_RADIUS, x+DRAW_RADIUS, y+DRAW_RADIUS);
					cr.SetRect(x+DRAW_RADIUS, y-DRAW_RADIUS, x+DRAW_RADIUS+DRAW_TEXT_WIDTH, y-DRAW_RADIUS+DRAW_TEXT_HEIGHT);
					//寻找该卡片是否是在保存的卡片中
					tag_index = FindTagIdInSave(m_listshowinfo[i].reciveinfo.TagId);
					if(tag_index == -1)
					{
						IdToString(str, m_listshowinfo[i].reciveinfo.TagId); 
					}
					else
					{
						str = m_saveinfo.tag[tag_index].Name;
						if(str.IsEmpty())
							IdToString(str, m_saveinfo.tag[tag_index].ID);
					}					
					::DrawText(m_mdc, str, -1, &cr, DT_VCENTER|DT_SINGLELINE);
				}
				break;
			case 1:		//右上角
				{
					if(m_listshowinfo[i].LastDrawSingle == m_listshowinfo[i].reciveinfo.Single)
					{
						//信号强度相同，不更新图标位置，使用上次的点坐标
						x = m_listshowinfo[i].LastDrawPoint.x;
						y = m_listshowinfo[i].LastDrawPoint.y;
					}
					else
					{					
						//随机产生一个坐标
						x = rand()%(m_listshowinfo[i].reciveinfo.Single);
						y = (int)sqrt((double)(m_listshowinfo[i].reciveinfo.Single * m_listshowinfo[i].reciveinfo.Single - x*x));
						
						x = DRAW_WIDTH-BMP_ROUTER_WIDTH-x;					
						y += BMP_ROUTER_HEIGHT;
					}

					Ellipse(m_mdc, x-DRAW_RADIUS, y-DRAW_RADIUS, x+DRAW_RADIUS, y+DRAW_RADIUS);
					cr.SetRect(x-DRAW_RADIUS-DRAW_TEXT_WIDTH, y-DRAW_RADIUS, x-DRAW_RADIUS, y-DRAW_RADIUS+DRAW_TEXT_HEIGHT);
					//寻找该卡片是否是在保存的卡片中
					tag_index = FindTagIdInSave(m_listshowinfo[i].reciveinfo.TagId);
					if(tag_index == -1)
					{
						IdToString(str, m_listshowinfo[i].reciveinfo.TagId); 
					}
					else
					{
						str = m_saveinfo.tag[tag_index].Name;
						if(str.IsEmpty())
							IdToString(str, m_saveinfo.tag[tag_index].ID);
					}					
					::DrawText(m_mdc, str, -1, &cr, DT_RIGHT|DT_VCENTER|DT_SINGLELINE);
				}
				break;
			case 2:		//右下角
				{
					if(m_listshowinfo[i].LastDrawSingle == m_listshowinfo[i].reciveinfo.Single)
					{
						//信号强度相同，不更新图标位置，使用上次的点坐标
						x = m_listshowinfo[i].LastDrawPoint.x;
						y = m_listshowinfo[i].LastDrawPoint.y;
					}
					else
					{					
						//随机产生一个坐标
						x = rand()%(m_listshowinfo[i].reciveinfo.Single);
						y = (int)sqrt((double)(m_listshowinfo[i].reciveinfo.Single * m_listshowinfo[i].reciveinfo.Single - x*x));
						
						x = DRAW_WIDTH-BMP_ROUTER_WIDTH-x;					
						y = DRAW_HEIGHT-BMP_ROUTER_HEIGHT-y;
					}

					Ellipse(m_mdc, x-DRAW_RADIUS, y-DRAW_RADIUS, x+DRAW_RADIUS, y+DRAW_RADIUS);
					cr.SetRect(x-DRAW_RADIUS-DRAW_TEXT_WIDTH, y-DRAW_RADIUS, x-DRAW_RADIUS, y-DRAW_RADIUS+DRAW_TEXT_HEIGHT);
					//寻找该卡片是否是在保存的卡片中
					tag_index = FindTagIdInSave(m_listshowinfo[i].reciveinfo.TagId);
					if(tag_index == -1)
					{
						IdToString(str, m_listshowinfo[i].reciveinfo.TagId); 
					}
					else
					{
						str = m_saveinfo.tag[tag_index].Name;
						if(str.IsEmpty())
							IdToString(str, m_saveinfo.tag[tag_index].ID);
					}					
					::DrawText(m_mdc, str, -1, &cr, DT_RIGHT|DT_VCENTER|DT_SINGLELINE);
				}
				break;
			case 3:		//左下角
				{
					if(m_listshowinfo[i].LastDrawSingle == m_listshowinfo[i].reciveinfo.Single)
					{
						//信号强度相同，不更新图标位置，使用上次的点坐标
						x = m_listshowinfo[i].LastDrawPoint.x;
						y = m_listshowinfo[i].LastDrawPoint.y;
					}
					else
					{					
						//随机产生一个坐标
						x = rand()%(m_listshowinfo[i].reciveinfo.Single);
						y = (int)sqrt((double)(m_listshowinfo[i].reciveinfo.Single * m_listshowinfo[i].reciveinfo.Single - x*x));
						
						x += BMP_ROUTER_WIDTH;				
						y = DRAW_HEIGHT-BMP_ROUTER_HEIGHT-y;
					}

					Ellipse(m_mdc, x-DRAW_RADIUS, y-DRAW_RADIUS, x+DRAW_RADIUS, y+DRAW_RADIUS);
					cr.SetRect(x+DRAW_RADIUS, y-DRAW_RADIUS, x+DRAW_RADIUS+DRAW_TEXT_WIDTH, y-DRAW_RADIUS+DRAW_TEXT_HEIGHT);
					//寻找该卡片是否是在保存的卡片中
					tag_index = FindTagIdInSave(m_listshowinfo[i].reciveinfo.TagId);
					if(tag_index == -1)
					{
						IdToString(str, m_listshowinfo[i].reciveinfo.TagId); 
					}
					else
					{
						str = m_saveinfo.tag[tag_index].Name;
						if(str.IsEmpty())
							IdToString(str, m_saveinfo.tag[tag_index].ID);
					}					
					::DrawText(m_mdc, str, -1, &cr, DT_VCENTER|DT_SINGLELINE);
				}
				break;
			}
			m_listshowinfo[i].LastDrawSingle = m_listshowinfo[i].reciveinfo.Single;
			m_listshowinfo[i].LastDrawPoint.x = x;
			m_listshowinfo[i].LastDrawPoint.y = y;
		}
	}


	BitBlt(m_hdc, DRAW_LEFT, DRAW_TOP, DRAW_WIDTH, DRAW_HEIGHT, m_mdc, 0, 0, SRCCOPY);

}
