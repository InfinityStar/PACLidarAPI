#include "lidarManager.h"

#define BEAM_ACCURACY 2

using namespace std;

lidarManager::lidarManager(string ip, uint16_t port)
{

    lidarParam.speed = PacLidar::SET_SPEED_HZ_10;
    lidarParam.dataType = PacLidar::SET_DATA_CHECKED;
    dataReceiver = 0;
    isCap = false;

    lidarSockAddr = new sockaddr_in();
    if ((lidarSockFD = socket(AF_INET, SOCK_STREAM, 0)) < 0){
        cerr << T_RED << "Error : Fata:Create socket failed:" << strerror(errno) << T_RESET << endl;
        exit(-1);
    }

    rcvtimeout.tv_sec  = 3;
    rcvtimeout.tv_usec = 0;
    setsockopt(lidarSockFD,SOL_SOCKET,SO_RCVTIMEO,&rcvtimeout,sizeof(rcvtimeout));

    bzero(lidarSockAddr, sizeof(struct sockaddr_in));
    lidarSockAddr->sin_family = AF_INET;
    lidarSockAddr->sin_addr.s_addr = inet_addr(ip.c_str());
    lidarSockAddr->sin_port = htons(port);

    pthread_mutexattr_init(&mutexattr);
    pthread_mutexattr_settype(&mutexattr, PTHREAD_MUTEX_ERRORCHECK);
    pthread_mutex_init(&mutex, &mutexattr);
    pthread_cond_init(&cond_CopyPkg,NULL);
}

lidarManager::~lidarManager()
{
    DBG_INFO << T_BLUE << "Disconnecting from lidar..." << T_RESET << endl;
    if(disconnectFromLidar()==0);
        DBG_INFO << T_GREEN << "Disconnected from lidar." << T_RESET << endl;

    if (lidarSockAddr != nullptr)
        delete lidarSockAddr;

    pthread_mutex_destroy(&mutex);
    pthread_cond_destroy(&cond_CopyPkg);
}

int lidarManager::connect2Lidar(uint32_t timeout_sec)
{
    DBG_INFO << T_BLUE << "Connecting to lidar..." << T_RESET << endl;
    bool foreverLoop = !timeout_sec;
    if(connect(lidarSockFD, (sockaddr *)lidarSockAddr, sizeof(struct sockaddr))==-1)
    {
        cerr<< T_RED << "Error : Failed to connect to lidar :" << strerror(errno) << T_RESET << endl
        <<T_BLUE<<"Retrying..."<<T_RESET << endl;
        sleep(1);
        int flag = fcntl(lidarSockFD, F_GETFL);
        fcntl(lidarSockFD, F_SETFL, flag | O_NONBLOCK);
        errno = 0;

        while (lidarSockFD > 0 && connect(lidarSockFD, (sockaddr *)lidarSockAddr, sizeof(struct sockaddr)) == -1)
        {
            if (errno != EAGAIN && errno!=EINPROGRESS && errno!=EALREADY) 
                cerr << T_RED << "Error : Failed to connect to lidar : " << strerror(errno) << T_RESET << endl
                <<T_BLUE<<"Retrying..."<< T_RESET << endl;
            sleep(1);
            if (!foreverLoop)
            {
                if (!(--timeout_sec))
                {
                    errno = ETIMEDOUT;
                    if (lidarSockFD > 0)
                        fcntl(lidarSockFD, F_SETFL, flag);
                    cerr << T_RED << "Error : Connect to lidar timeout : " << strerror(errno) << T_RESET << endl;
                    return -1;
                }
                else
                    continue;
            }
        }
        if (lidarSockFD > 0)
            fcntl(lidarSockFD, F_SETFL, flag);
    }

    DBG_INFO << T_GREEN << "The connection has been successful." << T_RESET << endl;
    return 0;
}

int lidarManager::reconnect2Lidar()
{
    close(lidarSockFD);
    if ((lidarSockFD = socket(AF_INET, SOCK_STREAM, 0)) < 0){
        cerr << T_RED << "Fata : Failed to reset socket fd. :" << strerror(errno) << T_RESET << endl;
        exit(-1);
    }
    setsockopt(lidarSockFD,SOL_SOCKET,SO_RCVTIMEO,&rcvtimeout,sizeof(rcvtimeout));
    return connect2Lidar();
}

int lidarManager::disconnectFromLidar()
{
    if (pthread_kill(dataReceiver,0)==0)
        stopLidar();
    if (lidarSockFD > 0)
        return close(lidarSockFD);
    else
        return 0;
}

int lidarManager::startupLidar()
{
    return startupLidar(lidarParam.speed,lidarParam.dataType);
}

int lidarManager::startupLidar(PacLidar::lidarCMD speed, PacLidar::lidarCMD data_type)
{
    lidarParam.dataType = data_type;
    lidarParam.speed = speed;
    send_cmd_to_lidar(data_type);
    send_cmd_to_lidar(speed);
    if (send_cmd_to_lidar(PacLidar::CTRL_START) < 0)
        return -1;

    if(isCap == false)
    {
        isCap = true;
        pthread_create(&dataReceiver, NULL, dataRecvFunc, (void *)this);
        while (pthread_kill(dataReceiver, 0) != 0)
        {
            DBG_INFO << T_BLUE << "Wait for receiver thread starting." << T_RESET << endl;
            msleep(1);
        }
    }
    return 0;
}

int lidarManager::stopLidar()
{
    if (pthread_kill(dataReceiver,0)==0){
        isCap = false;
        pthread_join(dataReceiver, NULL);
    }
    if(send_cmd_to_lidar(PacLidar::CTRL_STOP)<0)
        return -1;
    else
        return 0;
}

int lidarManager::setupLidar(PacLidar::lidarCMD speed,PacLidar::lidarCMD data_type)
{
    lidarParam.speed = speed;
    lidarParam.dataType = data_type;
    if(send_cmd_to_lidar(speed)==-1 || send_cmd_to_lidar(data_type)==-1){
        cerr<<T_RED<<"Error : Setup lidar failed : "<<strerror(errno)<<T_RESET << endl;
        return -1;
    }
    return 0;
}

int lidarManager::getLidarScanByBeam(float *ranges, float *intensities, unsigned start_beam, unsigned stop_beam)
{
    assert(start_beam >= 0);
    assert(stop_beam <= PAC_MAX_BEAMS - 1);
    assert(stop_beam > start_beam);

    memset(ranges,      0, (stop_beam + 1 - start_beam) * sizeof(float));
    memset(intensities, 0, (stop_beam + 1 - start_beam) * sizeof(float));
    if (pthread_mutex_lock(&mutex) == 0)
    {
        float range = 0;
        for (int i = start_beam; i <= stop_beam; ++i)
        {
            range = oneCircleData[i].part1 / 1000.0;
            if (range)
            {
                ranges[i] = range;
                intensities[i] = oneCircleData[i].part3;
            }
            else
            {
                ranges[i] = INFINITY;
                intensities[i] = 0;
            }
        }
        pthread_cond_signal(&cond_CopyPkg);
        pthread_mutex_unlock(&mutex);
        return 0;
    }
    else
        return -1;
}


int lidarManager::getLidarScanByAngle(float *ranges, float *intensities, float start_angle, float stop_angle)
{
    unsigned start_beam = start_angle/PAC_ANGLE_RESOLUTION;
    unsigned stop_beam = stop_angle/PAC_ANGLE_RESOLUTION;
    stop_beam-=1;
    return getLidarScanByBeam(ranges,intensities,start_beam,stop_beam);
}

int lidarManager::getLidarState(PacLidar::lidarState_t &state)
{
    if (lidarStatus.id != 0)
    {
        state = lidarStatus;
        return 0;
    }
    else
        return -1;
}

int lidarManager::send_cmd_to_lidar(uint16_t cmd)
{
    if (lidarSockFD < 0)
        return -1;
    uint16_t netCMD = htons(cmd);
    int rtn = send(lidarSockFD, &netCMD, sizeof(netCMD), 0);
    if (rtn < 0 || rtn != sizeof(netCMD)){
        cerr << T_RED << "Error : Send cmd failed with errno "<<errno<<" : "<< strerror(errno) << T_RESET << endl;
        if(errno == ECONNRESET){ //reset by peer
            DBG_INFO << T_BLUE <<"Connection reset by peer,trying to reconnect it..."<<T_RESET << endl;
            reconnect2Lidar();
            return send_cmd_to_lidar(cmd);
        }
    }
    else if (rtn == sizeof(netCMD))
        DBG_INFO << T_GREEN << "Send cmd : 0x" << std::hex << ntohs(netCMD) << " succed" << T_RESET << endl;
    msleep(50);
    return rtn;
}

void *lidarManager::dataRecvFunc(void *recver)
{
    lidarManager *rcvr = (lidarManager *)recver;
    rcvr->capLidarData();
}

void lidarManager::capLidarData()
{
    if (lidarSockFD < 0)
        return;
    ssize_t recvedNum = 0;
    uint32_t buf_start_idx = 0;
    pthread_mutex_trylock(&mutex);
    PacLidar::LidarData_t data_remains[PAC_NUM_OF_ONE_SCAN];
    bzero(data_remains,sizeof(data_remains));
    size_t remain_size = 0;
    while (isCap)
    {
        size_t leftNum = sizeof(sockDataPkg);
        auto index = (uint8_t *)(&sockDataPkg);
        bool pkgHd = false, pkgTl = false;
        while (leftNum > 0)
        {
            bzero(index, leftNum);

            recvedNum = recv(lidarSockFD, index, leftNum, 0);
            if (recvedNum > 0)
            {
                /************检查包头***********/
                if (!pkgHd && sockDataPkg[0].part1 == PacLidar::LIDAR_DATA_HEADER_T.part1)
                {
                    if (sockDataPkg[0].part2 == PacLidar::LIDAR_DATA_HEADER_T.part2 &&
                        sockDataPkg[0].part3 == PacLidar::LIDAR_DATA_HEADER_T.part3)
                    {
                        // DBG_INFO << "Got pkg header!" << T_RESET << endl;
                        pkgHd = true;
                    }
                }
                /*******************************/
                if (pkgHd)
                {
                    leftNum -= recvedNum;
                    index += recvedNum;
                }
            }
            else if (recvedNum == 0)
            {
                DBG_INFO << T_BLUE << "Connection reset by peer,Trying to reconnect it..." << T_RESET << endl;
                reconnect2Lidar();
                startupLidar();
            }
            else
            {
                cerr << T_RED << "Error : Receive msg from lidar failed with errno:"<< errno <<": "<< strerror(errno) << T_RESET << endl;
                if(errno == EAGAIN)
                {
                    cerr<< T_RED <<"Error : Server maybe broken,trying to reconnect it..."<<T_RESET << endl;
                    reconnect2Lidar();
                    startupLidar();
                }
            }
        }
        
        /**************检查包尾***************/
        if (sockDataPkg[PAC_NUM_OF_ONE_PKG - 1].part1 == PacLidar::LIDAR_DATA_TAIL_T.part1)
        {
            if (sockDataPkg[PAC_NUM_OF_ONE_PKG - 1].part2 == PacLidar::LIDAR_DATA_TAIL_T.part2 &&
                sockDataPkg[PAC_NUM_OF_ONE_PKG - 1].part3 == PacLidar::LIDAR_DATA_TAIL_T.part3)
                pkgTl = true;
        }
        /***********************************/

        /*************提取数据-开始******************/
        if (pkgHd && pkgTl)
        {
            /*************提取雷达状态-开始**********************/
            lidarStatus.temprature = (sockDataPkg[PAC_IDX_OF_TMP].part1 << 16) + sockDataPkg[PAC_IDX_OF_TMP].part2;
            lidarStatus.id = (sockDataPkg[PAC_IDX_OF_ID].part1 << 16) + sockDataPkg[PAC_IDX_OF_ID].part2;
            lidarStatus.version = (sockDataPkg[PAC_IDX_OF_VER].part1 << 16) + sockDataPkg[PAC_IDX_OF_VER].part2;
            lidarStatus.speed = sockDataPkg[PAC_IDX_OF_SPD].part2;
            /*************提取雷达状态-结束**********************/

            /*************提取扫描数据-开始**********************/
            //拷贝至scan数组
            memcpy(scanDataPkg, sockDataPkg + 1, sizeof(scanDataPkg));

            //初始化起始角度和结束角度寻找的标志位
            static bool dataAvalible = false;
            
            //提取上次结余的数据
            if (remain_size > 0)
            {
                for (int i=0;i<remain_size;i++)
                {
                    PacLidar::LidarData_t datai = data_remains[i];
                    if (datai.part2 < PAC_MAX_BEAMS)
                        oneCircleData[datai.part2] = datai;
                }
                bzero(data_remains, sizeof(data_remains));
                remain_size = 0;
            }

            //提取本次收到的数据
            for(int i=0;i<PAC_NUM_OF_ONE_SCAN;i++){
                PacLidar::LidarData_t datai = scanDataPkg[i];
                if (datai.part2 < PAC_MAX_BEAMS)
                {
                    if (i != 0)
                    {
                        //进行头尾计算，如果差值小于0,证明当前数据为新一周的数据
                        PacLidar::LidarData_t dataPre = scanDataPkg[i - 1];
                        if ((datai.part2 - dataPre.part2) < 0)
                        {
                            dataAvalible = true;
                            remain_size = PAC_NUM_OF_ONE_SCAN - i;
                            assert(remain_size>0 && remain_size<=PAC_NUM_OF_ONE_SCAN);
                            memcpy(data_remains, &scanDataPkg[i], remain_size * sizeof(PacLidar::LidarData_t));
                            break;
                        }
                    }
                    //检测数据碰撞，如有碰撞证明前一周数据已经结束，此方法针对上一种判断方法漏掉的情况
                    //情况：[当数组的最后一个元素为当前一周的最后一个元素，且此元素小于5760时]
                    if(datai.part1!=0 && oneCircleData[datai.part2].part1 != 0)
                    {
                        dataAvalible = true;
                        remain_size = PAC_NUM_OF_ONE_SCAN - i;
                        assert(remain_size > 0 && remain_size <= PAC_NUM_OF_ONE_SCAN);
                        memcpy(data_remains, &scanDataPkg[i], remain_size * sizeof(PacLidar::LidarData_t));
                        break;
                    }
                    oneCircleData[datai.part2] = datai;
                }
                else
                {
                    dataAvalible = true;
                    if(datai.part2 == PAC_MAX_BEAMS )
                        oneCircleData[0] = datai;
                    remain_size = PAC_NUM_OF_ONE_SCAN - i - 1;
                    assert(remain_size >= 0 && remain_size <= PAC_NUM_OF_ONE_SCAN);
                    if (remain_size > 0)
                        memcpy(data_remains, &scanDataPkg[i + 1], remain_size * sizeof(PacLidar::LidarData_t));
                    break;
                }
            }
            //提取一周数据完毕，解锁
            if (dataAvalible)
            {
                // DBG_INFO << "Got one full pkg." << T_RESET << endl;
                pointsFilter(oneCircleData,sizeof(oneCircleData));
                struct timespec ts;
                timespec_get(&ts,TIME_UTC);
                ts.tv_nsec += 10000000; //10ms
                if(ts.tv_nsec > 1000000000){ ts.tv_sec += 1; ts.tv_nsec-=1000000000;}
                pthread_cond_timedwait(&cond_CopyPkg,&mutex,&ts);
                pthread_mutex_unlock(&mutex);
                
                pthread_mutex_lock(&mutex);
                bzero(oneCircleData, sizeof(oneCircleData));
                //重置标志
                dataAvalible = false;
            }
            /*************提取扫描数据-结束******************/
        }
        /*************提取数据-结束******************/
    }
    pthread_mutex_unlock(&mutex);
}

void lidarManager::msleep(uint32_t msec)
{
    while (msec > 0)
    {
        usleep(1000);
        msec--;
    }
}

void lidarManager::pointsFilter(PacLidar::LidarData_t *points, size_t size)
{
    if(size>(5042*sizeof(PacLidar::LidarData_t)))
        points[5042].part1 = 0;
}