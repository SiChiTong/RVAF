/*
Stereo Vision Algorithm Framework, Copyright(c) 2016-2018, Peng Chao
算法的链表式顺序执行
*/

#include "Circuit.h"
#include "..\layer\Layer.h"
#include "..\layer\DataLayer.h"
#include "..\layer\RansacLayer.h"
#include "..\layer\CVMatchLayer.h"
#include "..\layer\CVPointLayer.h"
#include "..\layer\ECMatchLayer.h"
#include "..\layer\AdaboostLayer.h"
#include "..\layer\MilTrackLayer.h"
#include "..\layer\SgmMatchLayer.h"
#include "..\layer\SupixSegLayer.h"
#include "..\layer\BinoTrackLayer.h"
#include "..\layer\EadpMatchLayer.h"
#include "..\layer\MatrixMulLayer.h"
#include "..\layer\SurfPointLayer.h"
#include "..\layer\EularMatchLayer.h"
#include "..\layer\IAEstimateLayer.h"
#include "..\layer\ICPEstimateLayer.h"
#include "..\layer\NDTEstimateLayer.h"
#include "..\layer\CenterPointLayer.h"
#include "..\layer\CVDesciptorLayer.h"
#include "..\layer\StereoRectifyLayer.h"
#include "..\layer\TriangulationLayer.h"
#include "..\layer\SurfDescriptorLayer.h"

#include <WinBase.h>

namespace svaf{

string Circuit::time_id_ = "";
Figures<float> Circuit::sout_;

string GetTimeString();

// 构造函数，执行初始化
Circuit::Circuit(SvafTask& svafTask, bool gui_mode) : 
	layers_(svafTask), 
	guiMode_(gui_mode),
	useMapping_(gui_mode),
	linklist_(NULL), 
	svaf_(svafTask),
	id_(0){

	// 静态成员变量初始化
	Layer::figures = &sout_;
	Layer::id = &id_;
	Layer::task_type = SvafApp::NONE;
	Layer::gui_mode = guiMode_;
	Layer::pCir = this;
	pause_ms_ = svafTask.pause();
	world_.rectified = false;

	// 进程间通信初始化
	if (useMapping_){
		c_mutex_ = OpenEvent(MUTEX_ALL_ACCESS, false, "SVAF_GUI2ALG_CMD_MUTEX");
		c_fileMapping_ = OpenFileMapping(FILE_MAP_ALL_ACCESS, false, "SVAF_GUI2ALG_CMD");
		c_pMsg_ = (LPTSTR)MapViewOfFile(c_fileMapping_, FILE_MAP_ALL_ACCESS, 0, 0, 0);
		d_mutex_ = CreateEvent(nullptr, false, false, "SVAF_ALG2GUI_DATA_MUTEX");
		d_fileMapping_ = OpenFileMapping(FILE_MAP_ALL_ACCESS, false, "SVAF_ALG2GUI_DATA");
		d_pMsg_ = (LPTSTR)MapViewOfFile(d_fileMapping_, FILE_MAP_ALL_ACCESS, 0, 0, 0);
		i_mutex_ = CreateEvent(nullptr, false, false, "SVAF_ALG2GUI_INFO_MUTEX");
		i_fileMapping_ = OpenFileMapping(FILE_MAP_ALL_ACCESS, false, "SVAF_ALG2GUI_INFO");
		i_pMsg_ = (LPTSTR)MapViewOfFile(i_fileMapping_, FILE_MAP_ALL_ACCESS, 0, 0, 0);
		if (!c_mutex_ || !c_fileMapping_ || !c_pMsg_ || !d_mutex_ || !d_fileMapping_ || !d_pMsg_ || 
			!i_fileMapping_ || !i_pMsg_){
			useMapping_ = false;
		}
	}
	RLOG("SVAF opened.");
	Build();
	Run();
}

// 析构函数，结束时调用
Circuit::~Circuit(){
	// 删除算法链表
	Node *q, *p = linklist_;
	while (p){
		LOG(INFO) << "Destroy Layer [" << p->name << "].";
		q = p->next;
		delete p->layer;
		delete p;
		p = q;
	}
	// 释放立体矫正参数表
	StereoRectifyLayer::ReleaseTable();
	// 释放进程间通信资源
	if (useMapping_){
		if (!UnmapViewOfFile(c_fileMapping_)){}
		CloseHandle(c_fileMapping_);
		CloseHandle(c_mutex_);
		if (!UnmapViewOfFile(d_fileMapping_)){}
		CloseHandle(d_fileMapping_);
		CloseHandle(d_mutex_);
		if (!UnmapViewOfFile(i_fileMapping_)){}
		CloseHandle(i_fileMapping_);
		CloseHandle(i_mutex_);
	}
}

// 按照链表路径创建算法
void Circuit::Build(){
	// 创建之前先删除链表
	CHECK_GT(layers_.Size(), 0) << "Layer Count Error!";
	Node *p = linklist_, *q;
	while (p){
		LOG(INFO) << "Destroy Layer [" << p->name << "].";
		q = p->next;
		delete p->layer;
		delete p;
		p = q;
	}
	// 读入各个节点
	linklist_ = NULL;
	for (int i = 0; i < 1 + layers_.Size(); ++i){
		if (layers_[i].name() == layers_[i].bottom()){
			linklist_ = new Node(layers_[i].name());
			LOG(INFO) << linklist_->name << " -> ";
			break;
		}
		CHECK_EQ(i, layers_.Size() - 1) << "Not Found Start Layer!";
	}
	// 创建链表
	p = linklist_;
	while (true){
		if (layers_[p->name].name() == layers_[p->name].top()){
			LOG(INFO) << "Build All Layers.";
			break;
		}
		CHECK(layers_.IsLayerExit(layers_[p->name].top())) 
			<< layers_[p->name].top() << " is not exit!";
		p->next = new Node(layers_[p->name].top());
		LOG(INFO) << " -> " << p->next->name;
		p = p->next;
	}

	// 按照链表顺序实例化各个算法
	p = linklist_;
	Layer *layerinstance = NULL;
	void * param = NULL;
	while (p){
		auto type = layers_[p->name].type();
		auto layer = layers_[p->name];
		switch (type)
		{
		// 读入图像数据
		case svaf::LayerParameter_LayerType_NONE:
		case svaf::LayerParameter_LayerType_IMAGE:
		case svaf::LayerParameter_LayerType_IMAGE_PAIR:
		case svaf::LayerParameter_LayerType_VIDEO:
		case svaf::LayerParameter_LayerType_VIDEO_PAIR:
		case svaf::LayerParameter_LayerType_CAMERA:
		case svaf::LayerParameter_LayerType_CAMERA_PAIR:
		case svaf::LayerParameter_LayerType_DSP:
		case svaf::LayerParameter_LayerType_DSP_PAIR:
		case svaf::LayerParameter_LayerType_KINECT:
		case svaf::LayerParameter_LayerType_IMAGE_FOLDER:
		case svaf::LayerParameter_LayerType_IMAGE_PAIR_FOLDER:
			Layer::task_type = SvafApp::S_SHOW;
			layerinstance = new DataLayer(layer);
			break;
		// Adaboost目标检测
		case svaf::LayerParameter_LayerType_ADABOOST:
			Layer::task_type = SvafApp::S_DETECT;
			layerinstance = new AdaboostLayer(layer);
			break;
		// MIL目标跟踪
		case svaf::LayerParameter_LayerType_MILTRACK:
			Layer::task_type = SvafApp::S_DETECT;
			layerinstance = new MilTrackLayer(layer);
			break;
		// 双目目标跟踪
		case svaf::LayerParameter_LayerType_BITTRACK:
			Layer::task_type = SvafApp::S_DETECT;
			layerinstance = new BinoTrackLayer(layer);
			break;
		// 特征点检测
		case svaf::LayerParameter_LayerType_SIFT_POINT:
			Layer::task_type = SvafApp::S_POINT;
			break;
		case svaf::LayerParameter_LayerType_SURF_POINT:
			Layer::task_type = SvafApp::S_POINT;
			layerinstance = new SurfPointLayer(layer);
			break;
		case svaf::LayerParameter_LayerType_STAR_POINT:
			Layer::task_type = SvafApp::S_POINT;
			break;
		case svaf::LayerParameter_LayerType_BRISK_POINT:
			Layer::task_type = SvafApp::S_POINT;
			break;
		case svaf::LayerParameter_LayerType_FAST_POINT:
			Layer::task_type = SvafApp::S_POINT;
			break;
		case svaf::LayerParameter_LayerType_ORB_POINT:
			Layer::task_type = SvafApp::S_POINT;
			break;
		case svaf::LayerParameter_LayerType_KAZE_POINT:
			Layer::task_type = SvafApp::S_POINT;
			break;
		case svaf::LayerParameter_LayerType_HARRIS_POINT:
			Layer::task_type = SvafApp::S_POINT;
			break;
		case svaf::LayerParameter_LayerType_CV_POINT:
			Layer::task_type = SvafApp::S_POINT;
			layerinstance = new CVPointLayer(layer);
			break;
		// 特征描述
		case svaf::LayerParameter_LayerType_SIFT_DESP:
			Layer::task_type = SvafApp::S_POINTDESP;
			break;
		case svaf::LayerParameter_LayerType_SURF_DESP:
			Layer::task_type = SvafApp::S_POINTDESP;
			layerinstance = new SurfDescriptorLayer(layer);
			break;
		case svaf::LayerParameter_LayerType_STAR_DESP:
			Layer::task_type = SvafApp::S_POINTDESP;
			break;
		case svaf::LayerParameter_LayerType_BRIEF_DESP:
			Layer::task_type = SvafApp::S_POINTDESP;
			break;
		case svaf::LayerParameter_LayerType_CV_DESP:
			Layer::task_type = SvafApp::S_POINTDESP;
			layerinstance = new CVDesciptorLayer(layer);
			break;
		case svaf::LayerParameter_LayerType_BRISK_DESP:
			Layer::task_type = SvafApp::S_POINTDESP;
			break;
		case svaf::LayerParameter_LayerType_FAST_DESP:
			Layer::task_type = SvafApp::S_POINTDESP;
			break;
		case svaf::LayerParameter_LayerType_ORB_DESP:
			Layer::task_type = SvafApp::S_POINTDESP;
			break;
		case svaf::LayerParameter_LayerType_KAZE_DESP:
			Layer::task_type = SvafApp::S_POINTDESP;
			break;
		// 特征匹配
		case svaf::LayerParameter_LayerType_KDTREE_MATCH:
			Layer::task_type = SvafApp::POINT_MATCH;
			break;
		case svaf::LayerParameter_LayerType_EULAR_MATCH:
			Layer::task_type = SvafApp::POINT_MATCH;
			layerinstance = new EularMatchLayer(layer);
			break;
		case svaf::LayerParameter_LayerType_RANSAC:
			Layer::task_type = SvafApp::RANSAC_MATCH;
			layerinstance = new RansacLayer(layer);
			break;
		case svaf::LayerParameter_LayerType_BF_MATCH:
			Layer::task_type = SvafApp::RANSAC_MATCH;
			break;
		case svaf::LayerParameter_LayerType_FLANN_MATCH:
			Layer::task_type = SvafApp::RANSAC_MATCH;
			break;
		case svaf::LayerParameter_LayerType_EC_MATCH:
			Layer::task_type = SvafApp::RANSAC_MATCH;
			layerinstance = new ECMatchLayer(layer);
			break;
		case svaf::LayerParameter_LayerType_CV_MATCH:
			Layer::task_type = SvafApp::RANSAC_MATCH;
			layerinstance = new CVMatchLayer(layer);
			break;
		// 立体匹配
		case svaf::LayerParameter_LayerType_SGM_MATCH:
			Layer::task_type = SvafApp::STEREO_MATCH;
			layerinstance = new SgmMatchLayer(layer);
			break;
		case svaf::LayerParameter_LayerType_EADP_MATCH:
			Layer::task_type = SvafApp::STEREO_MATCH;
			layerinstance = new EadpMatchLayer(layer);
			break;
		// 三维重建，根据视差计算三维坐标
		case svaf::LayerParameter_LayerType_TRIANG:
			Layer::task_type = SvafApp::PC_TRIANGLE;
			layerinstance = new TriangulationLayer(layer);
			param = (void*)&world_;
			break;
		// 利用矩阵乘法进行三维空间坐标变换
		case svaf::LayerParameter_LayerType_MXMUL:
			Layer::task_type = SvafApp::PC_MULMATRIX;
			layerinstance = new MatrixMulLayer(layer);
			param = (void*)&world_;
			break;
		// 选取ROI区域中心位置
		case svaf::LayerParameter_LayerType_CENTER_POS:
			Layer::task_type = SvafApp::PR_CENTER;
			layerinstance = new CenterPointLayer(layer);
			param = (void*)&world_;
			break;
		// SAC-IA点云初始配准
		case svaf::LayerParameter_LayerType_IA_EST:
			Layer::task_type = SvafApp::PC_REGISTRATION;
			layerinstance = new IAEstimateLayer(layer);
			param = (void*)&world_;
			break;
		// ICP点云配准
		case svaf::LayerParameter_LayerType_IAICP_EST:
			Layer::task_type = SvafApp::PC_REGISTRATION;
			layerinstance = new ICPEstimateLayer(layer);
			param = (void*)&world_;
			break;
		// NDT点云配准
		case svaf::LayerParameter_LayerType_IANDT_EST:
			Layer::task_type = SvafApp::PC_REGISTRATION;
			layerinstance = new NDTEstimateLayer(layer);
			param = (void*)&world_;
			break;
		// 立体图像矫正
		case svaf::LayerParameter_LayerType_RECTIFY:
			Layer::task_type = SvafApp::S_RECTIFY;
			layerinstance = new StereoRectifyLayer(layer);
			param = (void*)&world_;
			break;
		// 超像素分割
		case svaf::LayerParameter_LayerType_SUPIX_SEG:
			Layer::task_type = SvafApp::S_SUPIX;
			layerinstance = new SupixSegLayer(layer);
			break;
		default:
			Layer::task_type = SvafApp::NONE;
			layerinstance = NULL;
			break;
		}

		CHECK_NOTNULL(layerinstance);
		p->layer = layerinstance;
		p->param = param;
		param = NULL;
		LOG(INFO) << "Layer [" << p->name << "] Builded.";
		
		p = p->next;
	}
	RLOG("All Layer Builded.");
}

// 执行算法
void Circuit::Run(){
	char buf[256] = {0};
	// 双目方式
	while (layers_.IsBinocular()){
		LOG(INFO) << "#Frame " << id_ << " Begin: ";

		Mat left, right;
		pair<Mat, Mat> matpair = make_pair(left, right);
		layers_ >> matpair;
		if (matpair.first.empty()){
			LOG(WARNING) << "Mat empty, Finish All Process.";
			sprintf(buf, "Frame %d Begin.", id_);
			RLOG(buf);
			break;
		}
		InitStep(); // 初始化
		images_.push_back(Block("left", matpair.first.clone()));
		images_.push_back(Block("right", matpair.second.clone()));
		RunStep(); // 运行
		if (!Disp()){
			break;
		}
		EndStep(); // 结束
		if (!ReciveCmd()){
			break;
		}
	}

	// 单目方式
	while (!layers_.IsBinocular()){
		LOG(INFO) << "#Frame " << id_ << " Begin: ";

		Mat mat;
		layers_ >> mat;
		if (mat.empty()){
			LOG(WARNING) << "Mat empty, Finish All Process.";
			sprintf(buf, "Frame %d Begin.", id_);
			RLOG(buf);
			break;
		}
		InitStep(); // 初始化
		images_.push_back(Block("left", mat.clone()));
		RunStep(); // 运行
		if (!Disp()){
			break;
		}
		EndStep(); // 结束
		
	}

	Analysis();
}


// 单帧执行
void Circuit::RunStep(){
	// 遍历算法链表执行算法
	void *param = NULL;
	Node *p = linklist_;
	while (p){
		LayerParameter layer = layers_[p->name];
		param = p->param;
		if (!p->layer->Run(images_, disp_, layer, param)){
			break;
		}
		p = p->next;
	}
	
}


// 显示输出
bool Circuit::Disp(){
	// 显示或保存图像
	for (int i = 0; i < disp_.size(); ++i){
		if (disp_[i].isShow){
			imshow(disp_[i].name, disp_[i].image);
		}
		if (disp_[i].isSave){
			imwrite(string("tmp/") + disp_[i].name + " " + Circuit::time_id_ 
				+ ".png", disp_[i].image);
		}
	}
	// 响应按键与帧间暂停
	if (pause_ms_ < 0)
		return true;
	char key = waitKey(pause_ms_);
	switch (key)
	{
	case 'q':
		return false;
		break;
	case 'e':
		exit(-1);
		break;
	case 'p':
		waitKey();
		break;
	case 'r':
		MilTrackLayer::reinit_ = true;
		break;
	default:
		break;
	}
	return true;
}

// 每帧处理前初始化
void Circuit::InitStep(){
	disp_.clear();
	images_.clear();
	world_.fetchtype = 0;
	world_.x = 0;
	world_.y = 0;
	world_.z = 0;
	world_.a = 0;
	world_.b = 0;
	world_.c = 0;

	char buf[256] = { 0 };
	sprintf(buf, "Frame %d Begin.", id_);
	RLOG(buf);

	char idch[5];
	sprintf(idch, "%04d", id_);
	string timestr = GetTimeString();
	string idstr(idch);
	Circuit::time_id_ = timestr + "_" + idstr;
	sout_.setRow(id_+1);
}

// 每帧结束操作
void Circuit::EndStep(){
	id_++;
	SendData();
	ReciveCmd();
	RLOG("Process Finished.");
}

// 数据记录与分析
void Circuit::Analysis(){
	RLOG("Analysis Data.");
	string timestr = GetTimeString();
	if (!layers_.images_.empty() && !layers_.IsBinocular()){
		sout_.print2txt_im(string("tmp/A_") + timestr + ".txt", layers_.images_);
	} else if (!layers_.imagepairs_.empty() && layers_.IsBinocular()){
		sout_.print2txt_impair(string("tmp/A_") + timestr + ".txt", layers_.imagepairs_);
	} else{
		sout_.print2txt(string("tmp/A_") + timestr + ".txt");
	}
	LOG(INFO) << "Analysis Data Has Been Saved: \n" << string("tmp/A_") + timestr + ".txt";
	if (id_ < 5){
		sout_.print2scr();
	}
	RLOG("SVAF closed.");
}

// 接受外部进程控制
bool Circuit::ReciveCmd(){
	if (!useMapping_){
		return true;
	}
	LPTSTR p = c_pMsg_;
	int cmd = ((int*)p)[0];
	if (cmd == 1){ // exit();
		LOG(INFO) << "Recived exit command.";
		return false;
	} else if (cmd == 2){
		RLOG("SVAF paused.");
		while (cmd != 3) {
			WaitForSingleObject(c_mutex_, INFINITE);
			cmd = ((int*)p)[0];
		}
		RLOG("SVAF continued.");
	}
	((int*)p)[0] = 0;
	return true;
}

// 向外部进程发送数据格式
using Bucket = struct{
	char	head[4];
	int		msgCount;
	int		imgCount;
	int		cols[8];
	int		rows[8];
	int		chns[8];
	int		offs[8];
	int		PointSize[4];
	int		PointChns[4];// xyz(3) or xyzrgb(6)
	int		PointOffs[4];
	int		pclCount;
	int		fetchtype; // 0 dont fetch, 1 world coord
	float	x;
	float	y;
	float	z;
	float	a;
	float	b;
	float	c;
};

// 向外部进程发送数据
void Circuit::SendData(){

	// 检查进程间通信资源是否创建
	if (!useMapping_ || !d_pMsg_){
		return;
	}

	LPTSTR p = d_pMsg_;
	char *pBuf = p;
	Bucket *pBucket = (Bucket*)pBuf;
	sprintf(pBucket->head, "pch");
	pBucket->msgCount = 1;

	// 按照协议发送图像和点云
	int frameCount = 0;
	int pointCount = 0;
	int offset = sizeof(Bucket);
	for (int i = 0; i < disp_.size(); ++i){
		if (disp_[i].isOutput && (!disp_[i].isOutput3DPoint) && (!disp_[i].image.empty()) && frameCount < 8){
			int cols = disp_[i].image.cols;
			int rows = disp_[i].image.rows;
			int chns = disp_[i].image.channels();
			int length = cols * rows * chns;
			memcpy(pBuf + offset, disp_[i].image.data, length);
			pBucket->cols[frameCount] = cols;
			pBucket->rows[frameCount] = rows;
			pBucket->chns[frameCount] = chns;
			pBucket->offs[frameCount] = offset;
			offset += (cols * rows * chns);
			frameCount++;
		}
		if (disp_[i].isOutput && disp_[i].isOutput3DPoint && pointCount < 4){
			int count = disp_[i].point3d.size();
			if (count <= 0){
				continue;
			}
			int chns = (disp_[i].color3d.size() == disp_[i].point3d.size()) ? 6 : 3;
			float *points = (float *)(pBuf + offset);
			if (chns == 3){
				for (int j = 0; j < count; ++j){
					points[j * 3 + 0] = disp_[i].point3d[j].x;
					points[j * 3 + 1] = disp_[i].point3d[j].y;
					points[j * 3 + 2] = disp_[i].point3d[j].z;
				}
			} else{
				for (int j = 0; j < count; ++j){
					points[j * 6 + 0] = disp_[i].point3d[j].x;
					points[j * 6 + 1] = disp_[i].point3d[j].y;
					points[j * 6 + 2] = disp_[i].point3d[j].z;
					points[j * 6 + 3] = disp_[i].color3d[j].r;
					points[j * 6 + 4] = disp_[i].color3d[j].g;
					points[j * 6 + 5] = disp_[i].color3d[j].b;
				}
			}
			memcpy(pBuf + offset, points, count * chns * sizeof(float));
			pBucket->PointSize[pointCount] = count;
			pBucket->PointChns[pointCount] = chns;
			pBucket->PointOffs[pointCount] = offset;
			offset += (count * chns * sizeof(float));
			pointCount++;
		}
	}
	pBucket->imgCount = frameCount;
	pBucket->pclCount = pointCount;
	// decition fetch
	pBucket->fetchtype = world_.fetchtype;
	pBucket->x = world_.x;
	pBucket->y = world_.y;
	pBucket->z = world_.z;
	pBucket->a = world_.a;
	pBucket->b = world_.b;
	pBucket->c = world_.c;

	// 通知外部进程数据已写入
	SetEvent(d_mutex_);
}

// 将信息字符串发送给外部进程
void Circuit::RLOG(std::string infoStr){

	if (!useMapping_ || !i_pMsg_){
		return;
	}

	LPTSTR p = i_pMsg_;
	char *pBuf = p;
	memcpy(pBuf, infoStr.data(), infoStr.length());
	pBuf[infoStr.length()] = '\0';
	PulseEvent(i_mutex_);
}

// 生成时间字符串，用于自动保存文件的文件名
string GetTimeString(){
	char filename[16];
	time_t rawtime;
	struct tm * t_tm;
	time(&rawtime);
	t_tm = localtime(&rawtime);
	int it = clock() % 1000;

	sprintf(filename, "%02d%02d%02d%02d%02d%02d%03d",
		t_tm->tm_year - 100, t_tm->tm_mon + 1, t_tm->tm_mday,
		t_tm->tm_hour, t_tm->tm_min, t_tm->tm_sec, it);
	string strFileName(filename);
	return strFileName;
}

}
