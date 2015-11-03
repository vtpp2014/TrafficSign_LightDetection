#include "HOG_ANN.h"
#include "traffic.h"
#include "math_utils.h"
#include "socket_server_task.h"
#include "Drogonfly_ImgRead.h"
#include "TrafficLightDetection/std_tlr.h"
#include <Windows.h>
#include <queue>


//TL HOG descriptors
Size Win_vertical(15,30),block_vertical(5,10),blockStride_vertical(5,5),cell_vertical(5,5);
HOGDescriptor myHOG_vertical(Win_vertical,block_vertical,blockStride_vertical,cell_vertical,9,1,-1.0,0,0.2,true,64);
HOGDescriptor myHOG_horz(Size(36,12),Size(12,6),Size(6,6),Size(6,6),9,1,-1.0,0,0.2,true,64);
int Frame_pos;//µ±Ç°Ö¡Î»ÖÃ

//control TSR_flag
bool isTrain=false;//traffic signs
bool TRAIN=false;//TL
bool HORZ=false;//TL
bool saveFlag=true;
Mat re_src;//for traffic signs detection 
IplImage *resize_TLR=cvCreateImage(Size(800,600),8,3);

vector<Rect> found_TL;//the bounding box for traffic lights
vector<Rect> found_TSR;//the bounding box for traffic signs
Scalar colorMode[]={CV_RGB(255,255,0),CV_RGB(0,0,255),CV_RGB(255,0,0)};//the color mode for the traffic sign detection(Y,B,R)
CvANN_MLP nnetwork,nnetwork_RoundRim,nnetwork_RectBlue;//neural networks for three different kinds of traffic signs 
PCA pca,pca_RoundRim,pca_RectBlue;
deque<float> signFilters[7];
deque<float> TLFilters[2];

//test function
void testCamera(PCA &pca,PCA &pca_RoundRim,PCA &pca_RectBlue,CvANN_MLP &nnetwork,
	CvANN_MLP &nnetwork_RoundRim,CvANN_MLP &nnetwork_RoundBlue);
void TLDetection();
void cameraMultiThread();
void videoMultiThread();
void openMP_MultiThreadVideo();
void openMP_MultiThreadCamera();

Point2d getBoxCenter(Rect &boundingBox){
	Point2d centerPoint;
	centerPoint.x=boundingBox.x+boundingBox.width/2;
	centerPoint.y=boundingBox.y+boundingBox.height/2;
	return centerPoint;
}

void getCentersFromBoxes(vector<Rect> &boundingBoxs,vector<Point2d> &centers)
{
	for(int i=0;i<boundingBoxs.size();i++)
	{
		Point2d center=getBoxCenter(boundingBoxs[i]);
		centers.push_back(center);
	}
}

Rect getSearchRegion(Point2d center,Size windowSize,Size imageSize)
{
	Rect searchRegion;
	int imgWidth=imageSize.width;
	int imgHeight=imageSize.height;
	int windowWidth=windowSize.width;
	int windowHeight=windowSize.height;
	//ºá×ø±ê
	if(center.x-windowWidth/2<0)
	{
		searchRegion.x=0;
		searchRegion.width=windowWidth/2+(int)(center.x);
	}
	else if(center.x+windowWidth/2>imgWidth)
	{
		searchRegion.x=center.x-windowWidth/2;
		searchRegion.width=windowWidth/2+imgWidth-center.x;
	}
	else
	{
		searchRegion.x=center.x-windowWidth/2;
		searchRegion.width=windowWidth;
	}
	//×Ý×ø±ê
	if(center.y-windowHeight/2<0)
	{
		searchRegion.y=0;
		searchRegion.height=windowHeight/2+(int)(center.y);
	}
	else if(center.y+windowHeight/2>imgHeight)
	{
		searchRegion.y=center.y-windowHeight/2;
		searchRegion.height=windowHeight/2+imgHeight-center.y;
	}
	else
	{
		searchRegion.y=center.y-windowHeight/2;
		searchRegion.height=windowHeight;
	}
	return searchRegion;
}

bool isContainSigns(Mat img,Rect searchRegion,float thresholdRatio)
{
	Mat searchMat=img(searchRegion);//cut the traffic signs
	int count=0;
	//visit every pixel
	int nRows=searchMat.rows;
	int nCols=searchMat.cols;
	if (searchMat.isContinuous())
	{
		nCols *= nRows;  
		nRows = 1;
	}

	for(int i=0;i<nRows;i++)
	{
		uchar* p=searchMat.ptr<uchar>(i);
		for(int j=0;j<nCols;j++)
		{
			if(p[j]==255)
				count++;
		}
	}
	if(count>thresholdRatio*nRows*nCols)
		return true;
	return false;
}

void covertImg2HOG(Mat img,vector<float> &descriptors,int windowWidth,int windowHeight)
{
	HOGDescriptor hog(Size(windowWidth,windowHeight),Size(10,10),Size(5,5),Size(5,5),9,1,-1.0,0,0.2,true,64);
	hog.compute(img,descriptors,Size(8,8));
	cout<<"HOG Descriptor size:"<<descriptors.size()<<endl;
}

//get the HOG features(float array) of each image in the specified folder
//imgWidth and the imgHeight is the size that the image will be resized to
int readdata(String path,int num_folder,String outputfile,int imgWidth,int imgHeight)
{
	fstream dataSet(outputfile.c_str(),ios::out);
	String img_num,txt_path,folder,img_path;
	stringstream SS_folder;
	Mat img;
	vector<float> pixelVector;
	float ClassId=0;
	int sampleNum=0;
	//folder ID loop
	for(int j=0;j<num_folder;j++)
	{
		//get the folder name
		SS_folder.clear();
		SS_folder<<j;
		SS_folder>>folder;
		txt_path=path+"\\"+folder+"\\description.txt";
		ifstream txt(txt_path);
		if (!txt)
		{
			cout<<"can't open the txt file!"<<endl;
			exit(1);
		}
		
		while(getline(txt,img_path))
		{
			sampleNum++;
			//read image
			img=imread(img_path);
			Mat resizedImg(imgHeight,imgWidth,CV_8UC3) ;
			resize(img,resizedImg,resizedImg.size());

			covertImg2HOG(resizedImg,pixelVector,imgWidth,imgHeight);
			int img_dim=pixelVector.size();
			for( int l=0 ; l < img_dim; l++)
			{	
				dataSet << pixelVector[l] << " ";
			}

			//class ID=0 means the negative samples
			dataSet << ClassId << "\n";
		}
		ClassId=ClassId+1.0;
	}
	dataSet.close();
	return sampleNum;
}

void shuffleDataSet(string path,string outputfile)
{
	// raw dataset file  8729(rows) * 4800(cols)  not yet shuffle  
	std::ifstream file(path);
	std::string line;

	Mat dataSet;
	int ligne =0;

	// vector of vector containing each line of the dataset file = each image pixels (1*4800)
	vector< vector<double> > vv;


	// iterates through the file to construct the vector vv
	while (std::getline(file, line))
	{
		std::istringstream iss(line);
		double n;
		int k = 0;

		vector<double> v;

		while (iss >> n)
		{ 	
			if( k == RESIZED_IMG_DIM +1) break; 
			v.push_back(n);
			k++;
		}

		vv.push_back(v);
		ligne ++ ;

		cout<<"num:"<<ligne<<endl;

	}
	cout<<"put done"<<endl;

	random_shuffle(vv.begin(), vv.end());


	int countPut=0;
	for( int i=0; i < vv.size(); i++)
	{ 
		countPut++;
		double* tab = &vv[i][0];
		Mat img(1,RESIZED_IMG_DIM +1,CV_64FC1,tab);
		dataSet.push_back(img);
		cout<<"countPut:"<<countPut<<endl;
	}
	FileStorage fs(outputfile,FileStorage::WRITE);   
	//fetch the file name(without".yml")
	replace(outputfile.begin(),outputfile.end(),'.',' ');
	stringstream iss(outputfile);
	string outputfileName;
	iss>>outputfileName;
	fs<< outputfileName<< dataSet;
	fs.release(); 
}

void savePCA(string filepath,string outputPath)
{
	Mat dataset;
	// load the shuffled dataSet  ( 8729(rows)  *  48001(cols) )  the last column for the image ClassId	
	FileStorage fs(filepath,FileStorage::READ);

	replace(filepath.begin(),filepath.end(),'.',' ');
	stringstream iss(filepath);
	string readfileName;
	iss>>readfileName;

	fs[readfileName] >> dataset ;
	// exclude the ClassId before performing PCA
	Mat data = dataset(Range::all(), Range(0,RESIZED_IMG_DIM));
	//  perform to retain 99%  of the variance
	PCA pca(data, Mat(), CV_PCA_DATA_AS_ROW , 1.0f);

	// save the model generated for  future uses.
	FileStorage pcaFile(outputPath,FileStorage::WRITE);
	pcaFile << "mean" << pca.mean;
	pcaFile << "e_vectors" << pca.eigenvectors;
	pcaFile << "e_values" << pca.eigenvalues;
	pcaFile.release();
	fs.release();
}


void TLDetectionPerFrame(IplImage *frame,float *TLDSend)
{
	IplImage *imageSeg=NULL,*imageNoiseRem =NULL;

	found_TL.clear();
	cvResize(frame,resize_TLR);

	imageSeg = colorSegmentationTL(resize_TLR);
	imageNoiseRem=noiseRemoval(imageSeg);
#if ISDEBUG_TL
	cvNamedWindow("imgseg");
	cvShowImage("imgseg",imageSeg);
	cvWaitKey(5);
	cvShowImage("imageNoiseRem",imageNoiseRem);
	cvWaitKey(5);
#endif
	//componentExtraction(imageNoiseRem,resize_TLR,TLDSend,found_TL);
	componentExtractionTL(imageNoiseRem,resize_TLR,TLDSend);
	cvReleaseImage(&imageSeg);
	cvReleaseImage(&imageNoiseRem);
	cvReleaseImage(&frame);
}

void TSRecognitionPerFrame(IplImage *frame,float *TSRSend)
{
	vector<ShapeRecResult> shapeResult;
	Mat src(frame);
	
	resize(src,re_src,Size(640,480));
	Mat bilateralImg;
	bilateralFilter(re_src,bilateralImg,7,7*2,7/2);
	int count=0;
#if ISDEBUG_TS
	namedWindow("bilateralImg");
	imshow("bilateralImg",bilateralImg);
	waitKey(5);
#endif
	Mat ihls_image = convert_rgb_to_ihls(bilateralImg);
	Mat nhs_image=convert_ihls_to_seg(ihls_image);

#if ISDEBUG_TS
	namedWindow("seg");
	imshow("seg",nhs_image);
	waitKey(5);
#endif

	Mat noiseremove;
	int erosion_size=1;
	int eroionType=MORPH_CROSS;
	Mat element = getStructuringElement( eroionType,Size( 2*erosion_size + 1, 2*erosion_size+1 ),Point( erosion_size, erosion_size ) );
	erode( nhs_image, noiseremove, element);
#if ISDEBUG_TS
	namedWindow("morph");
	imshow("morph",noiseremove);
	waitKey(2);
#endif
	Mat labeledImg=ShapeRecognize(noiseremove,shapeResult);

#if ISDEBUG_TS
	namedWindow("labeledImg");
	imshow("labeledImg",labeledImg);
	waitKey(5);
#endif
	int filterLen=shapeResult.size();
	if (filterLen!=0)
	{
#pragma omp parallel for
		for (int i=0;i<shapeResult.size();i++)
		{
			Rect boundingBox=shapeResult[i].box;
			Point leftup(boundingBox.x,boundingBox.y);
			Point rightdown(boundingBox.x+boundingBox.width,boundingBox.y+boundingBox.height);
			//	rectangle(re_src,leftup,rightdown,colorMode[2],2);
			Mat recognizeMat=re_src(boundingBox);//cut the traffic signs
			//int count=0;
			deque<float>::iterator it;

			//for different color, set different neural network
			if(shapeResult[i].shape==TRIANGLE&&shapeResult[i].color==Y_VALUE)//yellow
			{
				rectangle(re_src,leftup,rightdown,colorMode[0],2);
				int result=Recognize(nnetwork,pca,recognizeMat,TRIANGLE_CLASSES);
				//set the recognition result to the image
				switch(result)
				{
				case 1:
					setLabel(re_src,"work",boundingBox);
					//TSRSend[0]=1.0;break;
					signFilters[0].push_back(1.0);
					if (signFilters[0].size()>5)
						signFilters[0].pop_front();

					it=signFilters[0].begin();
					while (it<signFilters[0].end())
					{
						if(*it==1.0)count++;
						it++;
					}
					if((float)(count)/(float)signFilters[0].size()>=0.4)
					{
						TSRSend[0]=1.0;
						//cout<<"detected"<<endl;
					}
					else
					{
						TSRSend[0]=0.0;
						//cout<<"No detected"<<endl;
					}
					count=0;
					break;	

				case 2:
					setLabel(re_src,"man",boundingBox);
					//TSRSend[1]=2.0;break;
					signFilters[1].push_back(2.0);
					if (signFilters[1].size()>5)
						signFilters[1].pop_front();

					it=signFilters[1].begin();
					while (it<signFilters[1].end())
					{
						if(*it==2.0)count++;
						it++;
					}
					if((float)(count)/(float)signFilters[1].size()>=0.4)
					{
						TSRSend[1]=2.0;
						//cout<<"detected"<<endl;
					}
					else
					{
						TSRSend[1]=0.0;
						//cout<<"No detected"<<endl;
					}
					count=0;
					break;	

				case 0:
					setLabel(re_src,"other",boundingBox);
					break;
				default:
					break;
				}
			}
			else if(shapeResult[i].shape==RECTANGLE&&shapeResult[i].color==B_VALUE)//circle
			{
				rectangle(re_src,leftup,rightdown,colorMode[1],2);
				int result=Recognize(nnetwork_RectBlue,pca_RectBlue,recognizeMat,RECTBLUE_CLASSES);
				//set the recognition result to the image
				switch(result)
				{
				case 1:
					setLabel(re_src,"park",boundingBox);
					//TSRSend[3]=4.0;break;
					signFilters[2].push_back(3.0);
					if (signFilters[2].size()>5)
						signFilters[2].pop_front();
			
					it=signFilters[2].begin();
					while (it<signFilters[2].end())
					{
						if(*it==3.0)count++;
						it++;
					}
					if((float)(count)/(float)signFilters[2].size()>=0.4)
					{
						TSRSend[2]=3.0;
						//cout<<"detected"<<endl;
					}
					else
					{
						TSRSend[2]=0.0;
						//cout<<"No detected"<<endl;
					}
					count=0;
					break;	

				case 0:
					setLabel(re_src,"other",boundingBox);
					break;	

				default:
					break;
				}
			}
			//round rim(speed limit)
			else if(shapeResult[i].shape==CIRCLE&&shapeResult[i].color!=B_VALUE)//circle
			{
				rectangle(re_src,leftup,rightdown,colorMode[2],2);
				int result=Recognize(nnetwork_RoundRim,pca_RoundRim,recognizeMat,ROUNDRIM_CLASSES);
				//set the recognition result to the image

				switch(result)
				{
				case 1:
					setLabel(re_src,"10",boundingBox);
					//TSRSend[5]=6.0;break;
					signFilters[3].push_back(4.0);
					if (signFilters[3].size()>5)
						signFilters[3].pop_front();

					it=signFilters[3].begin();
					while (it<signFilters[3].end())
					{
						if(*it==4.0)count++;
						it++;
					}
					if((float)(count)/(float)signFilters[3].size()>=0.4)
					{
						TSRSend[3]=4.0;//send the 4.0 for parking signs
#if ISDEBUG_TS
						cout<<"The number of NoSound sign in the container:"<<count<<endl;
#endif
					}
					else
					{
						TSRSend[3]=0.0;
						//cout<<"No detected"<<endl;
					}
					count=0;
					break;	

				case 2:
					setLabel(re_src,"20",boundingBox);
					signFilters[4].push_back(5.0);
					if (signFilters[4].size()>5)
						signFilters[4].pop_front();
	
					it=signFilters[4].begin();
					while (it<signFilters[4].end())
					{
						if(*it==5.0)count++;
						it++;
					}
					if((float)(count)/(float)signFilters[4].size()>=0.4)
					{
						TSRSend[4]=5.0;
						//cout<<"detected"<<endl;
					}
					else
					{
						TSRSend[4]=0.0;
						//cout<<"No detected"<<endl;
					}
					count=0;
					break;	
					//TSRSend=7.0;break;
				case 0:
					setLabel(re_src,"other",boundingBox);
					break;
				default:
					break;
				}
			}
		}
	}
	else
	{
		//´¦ÀíÃ»ÓÐ¼ì²â½á¹ûµÄÇé¿ö
		for (int i=0;i<=4;i++)
		{
			signFilters[i].push_back(0);
			if (signFilters[i].size()>5)
				signFilters[i].pop_front();
			//cout<<TSRSend[i]<<" ";


			deque<float>::iterator it;
			int containCount=0;//¼ÆËãÈÝÆ÷ÖÐÓÐÐ§¼ì²â½á¹ûÊýÄ¿
			it=signFilters[i].begin();
			while (it<signFilters[i].end())
			{
				if((*it)==(float)(i+1))
					containCount++;
				it++;
			}
#if ISDEBUG_TS
			if (i==5)
			{
				cout<<"The number of NoSound sign in the container:"<<containCount<<endl;
			}
#endif
			if((float)(containCount)/(float)signFilters[i].size()>=0.4)
			{
				TSRSend[i]=(float)(i+1);
				//cout<<"detected"<<endl;
			}
			else
			{
				TSRSend[i]=0.0;
				//cout<<"No detected"<<endl;
			}

		}

	}
	shapeResult.clear();
}

int main()
{
	//socket
	SocketInit();
	g_mat = cvCreateMat(10, 1, CV_32FC1);//transmit data

	//TL detection HOG descriptor
	CvFont font; 
	cvInitFont(&font,CV_FONT_HERSHEY_SIMPLEX, .5, .5, 0, 1, 8);
	if(HORZ)
		hogSVMTrainTL(myHOG_horz,TRAIN,HORZ);
	else
		hogSVMTrainTL(myHOG_vertical,TRAIN,HORZ);

	//BP neural network training
	if(isTrain)
	{
		//triangle
		String path="D:\\JY\\JY_TrainingSamples\\chanshuTrafficSign\\triangle";
		int triangleNum=readdata(path,TRIANGLE_CLASSES,"triangle.txt",IMG_NEW_DIM,IMG_NEW_DIM);
		shuffleDataSet("triangle.txt","shuffleTriangle.yml");
		savePCA("shuffleTriangle.yml","pcaTriangle.yml");
		loadPCA("pcaTriangle.yml", pca);
		NeuralNetTrain("shuffleTriangle.yml","xmlTriangle.xml",pca,triangleNum,TRIANGLE_CLASSES);
		nnetwork.load("xmlTriangle.xml", "xmlTriangle");

		//RoundRim
		String path_RoundRim="D:\\JY\\JY_TrainingSamples\\chanshuTrafficSign\\RoundRim";
		int roundrimNum=readdata(path_RoundRim,ROUNDRIM_CLASSES,"RoundRim.txt",IMG_NEW_DIM,IMG_NEW_DIM);
		shuffleDataSet("RoundRim.txt","shuffleRoundRim.yml");
		savePCA("shuffleRoundRim.yml","pcaRoundRim.yml");
		loadPCA("pcaRoundRim.yml", pca_RoundRim);
		NeuralNetTrain("shuffleRoundRim.yml","xmlRoundRim.xml",pca_RoundRim,roundrimNum,ROUNDRIM_CLASSES);
		nnetwork_RoundRim.load("xmlRoundRim.xml", "xmlRoundRim");

		//BlueRect
		String path_RectBlue="D:\\JY\\JY_TrainingSamples\\chanshuTrafficSign\\RectBlue";
		int roundblueNum=readdata(path_RectBlue,RECTBLUE_CLASSES,"\RectBlue.txt",RECT_SIGN_WIDTH,RECT_SIGN_HEIGHT);
		shuffleDataSet("RectBlue.txt","shuffleRectBlue.yml");
		savePCA("shuffleRectBlue.yml","pcaRectBlue.yml");
		loadPCA("pcaRectBlue.yml", pca_RectBlue);
		NeuralNetTrain("shuffleRectBlue.yml","xmlRectBlue.xml",pca_RectBlue,roundblueNum,RECTBLUE_CLASSES);
		nnetwork_RectBlue.load("xmlRectBlue.xml", "xmlRectBlue");
	}else{
		loadPCA("pcaTriangle.yml", pca);
		loadPCA("pcaRoundRim.yml", pca_RoundRim);
		loadPCA("pcaRectBlue.yml", pca_RectBlue);
		nnetwork.load("xmlTriangle.xml", "xmlTriangle");
		nnetwork_RoundRim.load("xmlRoundRim.xml", "xmlRoundRim");
		nnetwork_RectBlue.load("xmlRectBlue.xml", "xmlRectBlue");
	}
	
	//test_RBYcolorMerge_Video();
	//testCamera(pca,pca_RoundRim,pca_RectBlue,nnetwork,nnetwork_RoundRim,nnetwork_RoundBlue);
	//cameraMultiThread();
	//videoMultiThread();
	//TLDetection();
	openMP_MultiThreadVideo();
	//openMP_MultiThreadCamera();
	cvReleaseMat(&g_mat);
	system("pause");
}


//用来寻找颜色各通道的范围，方便设置阈值
void findColorRange()
{
	//测试使用，待删除
	Mat testGreen=imread("D:\\JY\\JY_TrainingSamples\\color\\green\\4.jpg");
	Mat Hsv,HLS,gray;
	Mat HSVChannels[3],HLSChannels[3],BGRChannels[3];
	cvtColor(testGreen,Hsv,CV_BGR2HSV);
	cvtColor(testGreen,HLS,CV_BGR2HLS);
	cvtColor(testGreen,gray,CV_BGR2GRAY);


	split(Hsv,HSVChannels);
	split(HLS,HLSChannels);
	split(testGreen,BGRChannels);

	double max_H,min_H,max_S,min_S,max_V,min_V;
	double max_H1,min_H1,max_S1,min_S1,max_L1,min_L1;
	double max_R,min_R,max_G,min_G,max_B,min_B;
	double min_gray,max_gray;

	minMaxLoc(HSVChannels[0],&min_H,&max_H);
	minMaxLoc(HSVChannels[1],&min_S,&max_S);
	minMaxLoc(HSVChannels[2],&min_V,&max_V);

	minMaxLoc(HLSChannels[0],&min_H1,&max_H1);
	minMaxLoc(HLSChannels[1],&min_L1,&max_L1);
	minMaxLoc(HLSChannels[2],&min_S1,&max_S1);

	minMaxLoc(BGRChannels[0],&min_B,&max_B);
	minMaxLoc(BGRChannels[1],&min_G,&max_G);
	minMaxLoc(BGRChannels[2],&min_R,&max_R);

	minMaxLoc(gray,&min_gray,&max_gray);

	ofstream outfile;
	outfile.open("D:\\JY\\TrafficSignDetection\\TrafficSignDetection\\debugInfo\\GreenTestdebug.txt",ios::app);
	outfile<<"maxH:"<<max_H<<endl;
	outfile<<"minH:"<<min_H<<endl;
	outfile<<"maxS:"<<max_S<<endl;
	outfile<<"minS:"<<min_S<<endl;
	outfile<<"maxV:"<<max_V<<endl;
	outfile<<"minV:"<<min_V<<endl;
	outfile<<""<<endl;
	outfile<<"maxH1:"<<max_H1<<endl;
	outfile<<"minH1:"<<min_H1<<endl;
	outfile<<"maxL1:"<<max_L1<<endl;
	outfile<<"minL1:"<<min_L1<<endl;
	outfile<<"maxS1:"<<max_S1<<endl;
	outfile<<"minS1:"<<min_S1<<endl;
	outfile<<""<<endl;
	outfile<<"maxB:"<<max_B<<endl;
	outfile<<"minB:"<<min_B<<endl;
	outfile<<"maxG:"<<max_G<<endl;
	outfile<<"minG:"<<min_G<<endl;
	outfile<<"maxR:"<<max_R<<endl;
	outfile<<"minR:"<<min_R<<endl;
	outfile<<""<<endl;
	outfile<<"min_gray:"<<min_gray<<endl;
	outfile<<"max_gray:"<<max_gray<<endl;
	outfile.close();

}


void openMP_MultiThreadVideo()
{
	bool saveFlag=false;
	IplImage * frame,*copyFrame;
	float connectResult[10]={0,0,0,0,0,0,0,0,0,0};
	CvCapture * cap=cvCreateFileCapture("D:\\JY\\JY_TrainingSamples\\changshu data\\TL\\good5.avi");
	float startTime=1000*(float)getTickCount()/getTickFrequency();
	CvVideoWriter * writer=NULL;
	//findColorRange();

	if (saveFlag)
	{
		SYSTEMTIME stTime;
		GetLocalTime(&stTime);
		char *videoPath = new char[100];// "D:/123.dat";
		sprintf(videoPath, "ResultVideo//Video_%04d%02d%02d%02d%02d%02d.avi", stTime.wYear, stTime.wMonth, stTime.wDay, stTime.wHour, stTime.wMinute, stTime.wSecond);
		writer=cvCreateVideoWriter(videoPath,CV_FOURCC('X', 'V', 'I', 'D'), 20, cvGetSize(resize_TLR), 1);
	}

	while(1)
	{
		float TSRSend[7]={0,0,0,0,0,0,0};//store the traffic signs recognition result
		float TLDSend[2]={0,0};//store the traffic lights detection result

		int start=cvGetTickCount();
		frame=cvQueryFrame(cap);
		if(!frame)break;
		//MultiThread
#if ISDEBUG_TL
		//cvNamedWindow("imgseg");
#endif
		copyFrame=cvCreateImage(Size(frame->width,frame->height),frame->depth,frame->nChannels);
		cvCopy(frame,copyFrame);
#if OPENMP
#pragma omp parallel sections
		{
#pragma omp section
			{
				//TSR 
				TSRecognitionPerFrame(frame,TSRSend);
			}

#pragma omp section
			{
				//TL detection
				TLDetectionPerFrame(copyFrame,TLDSend);
			}
		}
#else
		TSRecognitionPerFrame(frame,TSRSend);
		TLDetectionPerFrame(copyFrame,TLDSend);
#endif
		//show the detection  result of TL
		cvNamedWindow("TL");
		cvShowImage("TL",resize_TLR);
		cvWaitKey(5);
		//show the detection  result of TSR
		namedWindow("TSR");
		imshow("TSR",re_src);
		waitKey(5);


		if (saveFlag)
		{
			cvWriteFrame(writer,resize_TLR);
			waitKey(2);
		}


#if ISDEBUG_TL
		ofstream outfile;
		outfile.open(debugTLPath,ios::app);
		outfile<<"================frame=================="<<endl;
		outfile.close();
#endif
		//get the union result
		for (int i=0;i<7;i++)
		{
			connectResult[i]=TSRSend[i];
		}
		for (int i=0;i<2;i++)
		{
			connectResult[7+i]=TLDSend[i];
		}

		for (int i=0;i<9;i++)
		{
			cout<<connectResult[i]<<" ";
		}

		//socket
		if (!gb_filled)
		{
			*(float *)CV_MAT_ELEM_PTR(*g_mat, 0, 0) = (int)(1000*(float)getTickCount()/getTickFrequency()-startTime)%1000;//time stamp,防止溢出		
			//put the result into the g_mat to transmit
			for (int i=1;i<=9;i++)
				*(float *)CV_MAT_ELEM_PTR(*g_mat, i, 0)=connectResult[i-1];
			gb_filled = true;
		}

		char c=waitKey(5);
		if (c==27)
		{
			cvReleaseCapture(&cap);
			cvReleaseImage(&resize_TLR);
			cvDestroyAllWindows();
			if (saveFlag)cvReleaseVideoWriter(&writer);
			break;
		}

		int end=cvGetTickCount();
		float time=(float)(end-start)/(cvGetTickFrequency()*1000000);
		cout<<"process time:"<<time<<endl;
	}
	if (saveFlag)cvReleaseVideoWriter(&writer);
	cvReleaseCapture(&cap);
	cvDestroyAllWindows();
}


void openMP_MultiThreadCamera()
{
	Drogonfly_ImgRead p;
	p.Camera_Intial();
#if IS_SAVE
	CvVideoWriter *writer = cvCreateVideoWriter("cameraCapture6.avi",CV_FOURCC('X','V','I','D'),10,Size(800,600),1);
#endif

	IplImage * src_frame,*copyFrame;
	float connectResult[9]={0,0,0,0,0,0,0,0,0};
	float startTime=1000*(float)getTickCount()/getTickFrequency();
	while(1)
	{
		src_frame=p.Camera2IplImage();
		float TSRSend[7]={0,0,0,0,0,0,0};//store the traffic signs recognition result
		float TLDSend[2]={0,0};//store the traffic lights detection result
		IplImage* frame=cvCreateImage(Size(800,600),src_frame->depth,src_frame->nChannels);
		int start=cvGetTickCount();
		if(!frame)break;
		//MultiThread
#if ISDEBUG_TL
		cvNamedWindow("imgseg");
#endif
		//copyFrame=cvCloneImage(frame);
		copyFrame=cvCreateImage(Size(frame->width,frame->height),frame->depth,frame->nChannels);
		cvResize(src_frame,frame);
		cvCopy(frame,copyFrame);

#if OPENMP
#pragma omp parallel sections
		{
#pragma omp section
			{
				//TSR 
				TSRecognitionPerFrame(frame,TSRSend);
			}

#pragma omp section
			{
				//TL detection
				TLDetectionPerFrame(copyFrame,TLDSend);
			}
		}
#else
		TSRecognitionPerFrame(frame,TSRSend);
		TLDetectionPerFrame(copyFrame,TLDSend);
#endif
		//show the detection  result of TL
		cvNamedWindow("TL");
		cvShowImage("TL",resize_TLR);
		cvWaitKey(5);
		//show the detection  result of TSR
		namedWindow("TSR");
		imshow("TSR",re_src);
		waitKey(5);
#if IS_SAVE
		cvWriteFrame(writer,frame);
#endif
		cvReleaseImage(&frame);
		//get the union result
		for (int i=0;i<7;i++)
		{
			connectResult[i]=TSRSend[i];
		}
		for (int i=0;i<2;i++)
		{
			connectResult[7+i]=TLDSend[i];
		}

		for (int i=0;i<9;i++)
		{
			cout<<connectResult[i]<<" ";
		}

		//socket
		if (!gb_filled)
		{
			*(float *)CV_MAT_ELEM_PTR(*g_mat, 0, 0) = (int)(1000*(float)getTickCount()/getTickFrequency()-startTime)%1000;//time stamp,防止溢出			
			//put the result into the g_mat to transmit
			for (int i=1;i<=9;i++)
				*(float *)CV_MAT_ELEM_PTR(*g_mat, i, 0)=connectResult[i-1];
			gb_filled = true;
		}

		char c=waitKey(5);
		if (c==27)
		{
			p.ClearBuffer();
#if IS_SAVE
			cvReleaseVideoWriter(&writer); 
#endif
			break;
		}
		
		int end=cvGetTickCount();
		float time=(float)(end-start)/(cvGetTickFrequency()*1000000);
		cout<<"process time:"<<time<<endl;
	}
	cvDestroyAllWindows();
}