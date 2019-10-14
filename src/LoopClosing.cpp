//
// Created by zss on 18-4-18.
//

#include "LoopClosing.h"
#include "Converter.h"
#include "Optimizer.h"
#include "ORBmatcher.h"
#include "Sim3Solver.h"

namespace myslam
{
LoopClosing::LoopClosing(Map *pMap, KeyFrameDatabase *pDB, ORBVocabulary *pVoc) : mbResetRequested(false), mbFinishRequested(false), mbFinished(true), mpMap(pMap),
                                                                                  mpKeyFrameDB(pDB), mpORBVocabulary(pVoc), mpMatchedKF(NULL), mLastLoopKFid(0), mbRunningGBA(false), mbFinishedGBA(true),
                                                                                  mbStopGBA(false), mpThreadGBA(NULL), mnFullBAIdx(0)
{
    mnCovisibilityConsistencyTh = 3;
}

void LoopClosing::SetTracker(Tracking *pTracker)
{
    mpTracker = pTracker;
}

void LoopClosing::SetLocalMapper(LocalMapping *pLocalMapper)
{
    mpLocalMapper = pLocalMapper;
}

void LoopClosing::Run()
{
    //cout<<"------loop------"<<endl;
    mbFinished = false;

    while (1)
    {
        // Check if there are keyframes in the queue
        if (CheckNewKeyFrames())
        {
            // Detect loop candidates and check covisibility consistency
            if (DetectLoop())
            {
                // Compute similarity transformation [sR|t]
                // In the stereo/RGBD case s=1
                if (ComputeSim3())
                {
                    //cout<<"CorrectLoop"<<endl;
                    // Perform loop fusion and pose graph optimization
                    CorrectLoop();
                }
            }
        }

        ResetIfRequested();

        if (CheckFinish())
            break;

        usleep(5000);
    }

    SetFinish();
}

void LoopClosing::InsertKeyFrame(KeyFrame *pKF)
{
    unique_lock<mutex> lock(mMutexLoopQueue);
    if (pKF->mnId != 0)
        mlpLoopKeyFrameQueue.push_back(pKF);
}

bool LoopClosing::CheckNewKeyFrames()
{
    unique_lock<mutex> lock(mMutexLoopQueue);
    return (!mlpLoopKeyFrameQueue.empty());
}

bool LoopClosing::DetectLoop()
{
    {
        unique_lock<mutex> lock(mMutexLoopQueue);
        mpCurrentKF = mlpLoopKeyFrameQueue.front();
        mlpLoopKeyFrameQueue.pop_front(); //TODO list
        // Avoid that a keyframe can be erased while it is being process by this thread
        mpCurrentKF->SetNotErase();
    }

    //TODO If the map contains less than 10 KF or less than 10 KF have passed from last loop detection
    if (mpCurrentKF->mnId < mLastLoopKFid + 10)
    {
        mpKeyFrameDB->add(mpCurrentKF);
        mpCurrentKF->SetErase();
        return false;
    }

    // TODO Compute reference BoW similarity score
    // This is the lowest score to a connected keyframe in the covisibility graph
    // We will impose loop candidates to have a higher similarity than this
    const vector<KeyFrame *> vpConnectedKeyFrames = mpCurrentKF->GetVectorCovisibleKeyFrames();
    const DBoW2::BowVector &CurrentBowVec = mpCurrentKF->mBowVec;
    float minScore = 1;
    for (size_t i = 0; i < vpConnectedKeyFrames.size(); i++)
    {
        KeyFrame *pKF = vpConnectedKeyFrames[i];
        if (pKF->isBad())
            continue;
        const DBoW2::BowVector &BowVec = pKF->mBowVec;

        float score = mpORBVocabulary->score(CurrentBowVec, BowVec);

        if (score < minScore)
            minScore = score;
    }

    // TODO Query the database imposing the minimum score
    vector<KeyFrame *> vpCandidateKFs = mpKeyFrameDB->DetectLoopCandidates(mpCurrentKF, minScore);

    // If there are no loop candidates, just add new keyframe and return false
    if (vpCandidateKFs.empty())
    {
        mpKeyFrameDB->add(mpCurrentKF);
        mvConsistentGroups.clear();
        mpCurrentKF->SetErase();
        return false;
    }

    // For each loop candidate check consistency with previous loop candidates
    // Each candidate expands a covisibility group (keyframes connected to the loop candidate in the covisibility graph)
    // A group is consistent with a previous group if they share at least a keyframe
    // We must detect a consistent loop in several consecutive keyframes to accept it
    mvpEnoughConsistentCandidates.clear();

    //TODO vpCandidateKFs
    //TODO mvConsistentGroups
    //TODO spCandidateeGroup
    vector<ConsistentGroup> vCurrentConsistentGroups;
    vector<bool> vbConsistentGroup(mvConsistentGroups.size(), false);
    for (size_t i = 0, iend = vpCandidateKFs.size(); i < iend; i++)
    {
        KeyFrame *pCandidateKF = vpCandidateKFs[i]; //TODO 回环候选关键帧

        set<KeyFrame *> spCandidateGroup = pCandidateKF->GetConnectedKeyFrames();
        spCandidateGroup.insert(pCandidateKF);

        bool bEnoughConsistent = false;
        bool bConsistentForSomeGroup = false;
        for (size_t iG = 0, iendG = mvConsistentGroups.size(); iG < iendG; iG++)
        {
            set<KeyFrame *> sPreviousGroup = mvConsistentGroups[iG].first;

            bool bConsistent = false;
            for (set<KeyFrame *>::iterator sit = spCandidateGroup.begin(), send = spCandidateGroup.end(); sit != send; sit++)
            {
                if (sPreviousGroup.count(*sit))
                {
                    bConsistent = true;
                    bConsistentForSomeGroup = true;
                    break;
                }
            }

            if (bConsistent) //TODO 都出现了的关键帧
            {
                int nPreviousConsistency = mvConsistentGroups[iG].second;
                int nCurrentConsistency = nPreviousConsistency + 1;
                if (!vbConsistentGroup[iG])
                {
                    ConsistentGroup cg = make_pair(spCandidateGroup, nCurrentConsistency);
                    vCurrentConsistentGroups.push_back(cg);
                    vbConsistentGroup[iG] = true; //TODO this avoid to include the same group more than once
                }
                if (nCurrentConsistency >= mnCovisibilityConsistencyTh && !bEnoughConsistent)
                {
                    mvpEnoughConsistentCandidates.push_back(pCandidateKF);
                    //cout<<"回环候选："<<pCandidateKF->mnId<<endl;
                    bEnoughConsistent = true; //TODO this avoid to insert the same candidate more than once
                }
            }
        }

        // TODO If the group is not consistent with any previous group insert with consistency counter set to zero
        if (!bConsistentForSomeGroup)
        {
            ConsistentGroup cg = make_pair(spCandidateGroup, 0); //TODO 为什么不是 1 呢
            vCurrentConsistentGroups.push_back(cg);
        }
    }

    // Update Covisibility Consistent Groups
    mvConsistentGroups = vCurrentConsistentGroups;

    // Add Current Keyframe to database
    mpKeyFrameDB->add(mpCurrentKF);

    //cout<<"候选个数："<<mvpEnoughConsistentCandidates.size()<<endl;
    if (mvpEnoughConsistentCandidates.empty())
    {
        mpCurrentKF->SetErase();
        return false;
    }
    else
    {
        return true;
    }

    mpCurrentKF->SetErase();
    return false;
}

bool LoopClosing::ComputeSim3()
{
    // For each consistent loop candidate we try to compute a Sim3

    const int nInitialCandidates = mvpEnoughConsistentCandidates.size(); //TODO mvpEnoughConsistentCandidates候选帧

    // We compute first ORB matches for each candidate
    // If enough matches are found, we setup a Sim3Solver
    ORBmatcher matcher(0.75, true);

    vector<Sim3Solver *> vpSim3Solvers;
    vpSim3Solvers.resize(nInitialCandidates);

    vector<vector<MapPoint *>> vvpMapPointMatches; //TODO 每个候选帧的匹配地图点
    vvpMapPointMatches.resize(nInitialCandidates);

    vector<bool> vbDiscarded; //TODO 1:bad 2:nmatches<20 3:bNoMore
    vbDiscarded.resize(nInitialCandidates);

    int nCandidates = 0; //candidates with enough matches

    for (int i = 0; i < nInitialCandidates; i++)
    {
        KeyFrame *pKF = mvpEnoughConsistentCandidates[i];

        // avoid that local mapping erase it while it is being processed in this thread
        pKF->SetNotErase();

        if (pKF->isBad())
        {
            vbDiscarded[i] = true;
            continue;
        }

        int nmatches = matcher.SearchByBoW(mpCurrentKF, pKF, vvpMapPointMatches[i]); //TODO 匹配上的地图点

        if (nmatches < 20)
        {
            vbDiscarded[i] = true;
            continue;
        }
        else
        {
            Sim3Solver *pSolver = new Sim3Solver(mpCurrentKF, pKF, vvpMapPointMatches[i], mbFixScale);
            pSolver->SetRansacParameters(0.99, 20, 300);
            vpSim3Solvers[i] = pSolver; //TODO 针对每个pkf建立sim求解器，地图点放在vvpMapPointMatches[i]
        }

        nCandidates++; //TODO 可以计算sim3的都算作候选
    }

    bool bMatch = false;

    // Perform alternatively RANSAC iterations for each candidate
    // until one is succesful or all fail
    while (nCandidates > 0 && !bMatch)
    {
        for (int i = 0; i < nInitialCandidates; i++) //TODO 大候选
        {
            if (vbDiscarded[i])
                continue;
            KeyFrame *pKF = mvpEnoughConsistentCandidates[i];
            // Perform 5 Ransac Iterations
            vector<bool> vbInliers;
            int nInliers;
            bool bNoMore;

            Sim3Solver *pSolver = vpSim3Solvers[i];
            cv::Mat Scm = pSolver->iterate(5, bNoMore, vbInliers, nInliers); //TODO 开始迭代计算，包含computesim3

            // TODO If Ransac reachs max. iterations discard keyframe
            if (bNoMore)
            {
                vbDiscarded[i] = true;
                nCandidates--;
            }
            // TODO If RANSAC returns a Sim3, perform a guided matching and optimize with all correspondences
            if (!Scm.empty())
            {
                vector<MapPoint *> vpMapPointMatches(vvpMapPointMatches[i].size(), static_cast<MapPoint *>(NULL));
                for (size_t j = 0, jend = vbInliers.size(); j < jend; j++)
                {
                    if (vbInliers[j])
                        vpMapPointMatches[j] = vvpMapPointMatches[i][j]; //TODO 匹配的地图点中的内点
                }

                //TODO 都是迭代计算出来的
                cv::Mat R = pSolver->GetEstimatedRotation();
                cv::Mat t = pSolver->GetEstimatedTranslation();
                const float s = pSolver->GetEstimatedScale();

                //TODO sim3就是两帧之间的转换
                matcher.SearchBySim3(mpCurrentKF, pKF, vpMapPointMatches, s, R, t, 7.5); //TODO 用处在哪

                g2o::Sim3 gScm(Converter::toMatrix3d(R), Converter::toVector3d(t), s);
                const int nInliers = Optimizer::OptimizeSim3(mpCurrentKF, pKF, vpMapPointMatches, gScm, 10, mbFixScale); //TODO 优化sim3

                // If optimization is succesful stop ransacs and continue
                if (nInliers >= 20)
                {
                    bMatch = true;
                    mpMatchedKF = pKF;
                    // cout<<"闭环关键帧号："<<mpMatchedKF->mnId<<endl;
                    g2o::Sim3 gSmw(Converter::toMatrix3d(pKF->GetRotation()), Converter::toVector3d(pKF->GetTranslation()), 1.0);
                    mg2oScw = gScm * gSmw;              //TODO 当前帧相对于参考帧的转换*参考帧本身的转换
                    mScw = Converter::toCvMat(mg2oScw); //TODO 相对转换--->绝对转换（相对于世界坐标系）

                    mvpCurrentMatchedPoints = vpMapPointMatches;
                    //TODO 注意这里得到的当前关键帧中匹配上闭环关键帧共视地图点（mvpCurrentMatchedPoints），将用于后面CorrectLoop时当前关键帧地图点的冲突融合
                    break;
                }
            }
        }
    }

    if (!bMatch)
    {
        for (int i = 0; i < nInitialCandidates; i++)
            mvpEnoughConsistentCandidates[i]->SetErase();
        mpCurrentKF->SetErase();
        return false;
    }

    //TODO 获取之前的回环帧的一些地图点
    // Retrieve MapPoints seen in Loop Keyframe and neighbors
    vector<KeyFrame *> vpLoopConnectedKFs = mpMatchedKF->GetVectorCovisibleKeyFrames();
    vpLoopConnectedKFs.push_back(mpMatchedKF);
    mvpLoopMapPoints.clear();
    for (vector<KeyFrame *>::iterator vit = vpLoopConnectedKFs.begin(); vit != vpLoopConnectedKFs.end(); vit++)
    {
        KeyFrame *pKF = *vit;
        vector<MapPoint *> vpMapPoints = pKF->GetMapPointMatches();
        for (size_t i = 0, iend = vpMapPoints.size(); i < iend; i++)
        {
            MapPoint *pMP = vpMapPoints[i];
            if (pMP)
            {
                if (!pMP->isBad() && pMP->mnLoopPointForKF != mpCurrentKF->mnId)
                {
                    mvpLoopMapPoints.push_back(pMP);
                    pMP->mnLoopPointForKF = mpCurrentKF->mnId; //防止重复添加
                }
            }
        }
    }

    //TODO 在当前关键帧中匹配所有关键帧中的地图点，需要计算sim3
    matcher.SearchByProjection(mpCurrentKF, mScw, mvpLoopMapPoints, mvpCurrentMatchedPoints, 10); //TODO最后看这个匹配点个数决定回环是否成功

    // If enough matches accept Loop
    int nTotalMatches = 0;
    for (size_t i = 0; i < mvpCurrentMatchedPoints.size(); i++)
    {
        if (mvpCurrentMatchedPoints[i])
            nTotalMatches++;
    }

    if (nTotalMatches >= 40)
    {
        for (int i = 0; i < nInitialCandidates; i++)
            if (mvpEnoughConsistentCandidates[i] != mpMatchedKF)
                mvpEnoughConsistentCandidates[i]->SetErase();
        return true;
    }
    else
    {
        for (int i = 0; i < nInitialCandidates; i++)
            mvpEnoughConsistentCandidates[i]->SetErase();
        mpCurrentKF->SetErase();
        return false;
    }
}

//TODO 在上一步求得了Sim3和对应点之后，就纠正了当前帧的位姿，但是我们的误差不仅仅在当前帧，此前的每一帧都有累计误差需要消除，所以这个函数CorrectLoop就是用来消除这个累计误差，进行整体的调节
void LoopClosing::CorrectLoop()
{
    cout << "Loop detected!" << endl;

    // Send a stop signal to Local Mapping
    // Avoid new keyframes are inserted while correcting the loop
    mpLocalMapper->RequestStop();

    // If a Global Bundle Adjustment is running, abort it
    if (isRunningGBA())
    {
        unique_lock<mutex> lock(mMutexGBA);
        mbStopGBA = true;

        mnFullBAIdx++;

        if (mpThreadGBA)
        {
            mpThreadGBA->detach();
            delete mpThreadGBA;
        }
    }

    // Wait until Local Mapping has effectively stopped
    while (!mpLocalMapper->isStopped())
    {
        usleep(1000);
    }

    // Ensure current keyframe is updated
    mpCurrentKF->UpdateConnections();

    // Retrive keyframes connected to the current keyframe and compute corrected Sim3 pose by propagation
    mvpCurrentConnectedKFs = mpCurrentKF->GetVectorCovisibleKeyFrames();
    mvpCurrentConnectedKFs.push_back(mpCurrentKF);

    //TODO 使用计算出的Sim3对当前位姿进行调整，并且传播到当前帧相连的的关键帧
    //TODO (相连关键帧之间相对位姿是知道的，通过当前帧的Sim3计算相连关键帧的Sim3).
    //TODO 这样回环的两侧关键帧就对齐了，利用调整过的位姿更新这些向量关键帧对应的地图点
    KeyFrameAndPose CorrectedSim3, NonCorrectedSim3; //TODO pair<const KeyFrame*, g2o::Sim3>
    CorrectedSim3[mpCurrentKF] = mg2oScw;
    cv::Mat Twc = mpCurrentKF->GetPoseInverse(); //[TODO return Twc
    {
        // Get Map Mutex
        unique_lock<mutex> lock(mpMap->mMutexMapUpdate);
        for (vector<KeyFrame *>::iterator vit = mvpCurrentConnectedKFs.begin(), vend = mvpCurrentConnectedKFs.end(); vit != vend; vit++)
        {
            KeyFrame *pKFi = *vit;
            cv::Mat Tiw = pKFi->GetPose();
            if (pKFi != mpCurrentKF) //TODO 关键帧可以直接相等，地图点也可以
            {
                cv::Mat Tic = Tiw * Twc;
                cv::Mat Ric = Tic.rowRange(0, 3).colRange(0, 3);
                cv::Mat tic = Tic.rowRange(0, 3).col(3);
                g2o::Sim3 g2oSic(Converter::toMatrix3d(Ric), Converter::toVector3d(tic), 1.0);
                g2o::Sim3 g2oCorrectedSiw = g2oSic * mg2oScw;
                CorrectedSim3[pKFi] = g2oCorrectedSiw; ///TODO CorrectedSim3
            }
            cv::Mat Riw = Tiw.rowRange(0, 3).colRange(0, 3);
            cv::Mat tiw = Tiw.rowRange(0, 3).col(3);
            g2o::Sim3 g2oSiw(Converter::toMatrix3d(Riw), Converter::toVector3d(tiw), 1.0);
            NonCorrectedSim3[pKFi] = g2oSiw; ///TODO NonCorrectedSim3
        }

        // TODO 用纠正后的坐标更新地图点和关键帧的pose
        for (KeyFrameAndPose::iterator mit = CorrectedSim3.begin(), mend = CorrectedSim3.end(); mit != mend; mit++)
        {
            KeyFrame *pKFi = mit->first;
            g2o::Sim3 g2oCorrectedSiw = mit->second;
            g2o::Sim3 g2oCorrectedSwi = g2oCorrectedSiw.inverse();

            g2o::Sim3 g2oSiw = NonCorrectedSim3[pKFi];

            vector<MapPoint *> vpMPsi = pKFi->GetMapPointMatches(); //TODO 向量
            //TODO Correct all MapPoints obsrved by current keyframe and neighbors, so that they align with the other side of the loop
            for (size_t iMP = 0, endMPi = vpMPsi.size(); iMP < endMPi; iMP++)
            {
                MapPoint *pMPi = vpMPsi[iMP];
                if (!pMPi)
                    continue;
                if (pMPi->isBad())
                    continue;
                if (pMPi->mnCorrectedByKF == mpCurrentKF->mnId)
                    continue;

                //TODO Project with non-corrected pose and project back with corrected pose
                cv::Mat P3Dw = pMPi->GetWorldPos();
                Eigen::Matrix<double, 3, 1> eigP3Dw = Converter::toVector3d(P3Dw);
                Eigen::Matrix<double, 3, 1> eigCorrectedP3Dw = g2oCorrectedSwi.map(g2oSiw.map(eigP3Dw));

                cv::Mat cvCorrectedP3Dw = Converter::toCvMat(eigCorrectedP3Dw);
                pMPi->SetWorldPos(cvCorrectedP3Dw); //TODO 更新地图点纠正后的位置,Mat，完事更新观测和深度
                pMPi->mnCorrectedByKF = mpCurrentKF->mnId;
                pMPi->mnCorrectedReference = pKFi->mnId;
                pMPi->UpdateNormalAndDepth();
            }

            // TODO Update keyframe pose with corrected Sim3. First transform Sim3 to SE3 (scale translation)
            Eigen::Matrix3d eigR = g2oCorrectedSiw.rotation().toRotationMatrix();
            Eigen::Vector3d eigt = g2oCorrectedSiw.translation();
            double s = g2oCorrectedSiw.scale();

            eigt *= (1. / s); //[R t/s;0 1]

            cv::Mat correctedTiw = Converter::toCvSE3(eigR, eigt);

            pKFi->SetPose(correctedTiw); //TODO 更新关键纠正后的位置,SE3,Mat,完事更新链接

            // Make sure connections are updated
            pKFi->UpdateConnections();
        }

        // TODO Start Loop Fusion
        // TODO Update matched map points and replace if duplicated
        for (size_t i = 0; i < mvpCurrentMatchedPoints.size(); i++)
        {
            if (mvpCurrentMatchedPoints[i])
            {
                MapPoint *pLoopMP = mvpCurrentMatchedPoints[i];
                MapPoint *pCurMP = mpCurrentKF->GetMapPoint(i); //TODO mvpMapPoints[i]
                if (pCurMP)
                    pCurMP->Replace(pLoopMP);
                else
                {
                    mpCurrentKF->AddMapPoint(pLoopMP, i);
                    pLoopMP->AddObservation(mpCurrentKF, i);
                    pLoopMP->ComputeDistinctiveDescriptors();
                }
            }
        }
    }

    // Project MapPoints observed in the neighborhood of the loop keyframe
    // into the current keyframe and neighbors using corrected poses.
    // Fuse duplications.
    SearchAndFuse(CorrectedSim3);

    // TODO After the MapPoint fusion, new links in the covisibility graph will appear attaching both sides of the loop
    map<KeyFrame *, set<KeyFrame *>> LoopConnections;
    for (vector<KeyFrame *>::iterator vit = mvpCurrentConnectedKFs.begin(), vend = mvpCurrentConnectedKFs.end(); vit != vend; vit++)
    {
        KeyFrame *pKFi = *vit;
        vector<KeyFrame *> vpPreviousNeighbors = pKFi->GetVectorCovisibleKeyFrames(); //TODO 二级共视关键帧
        // Update connections. Detect new links.
        pKFi->UpdateConnections();                             //TODO 因为上一步更新了地图点
        LoopConnections[pKFi] = pKFi->GetConnectedKeyFrames(); //TODO 《关键帧,链接的关键帧》
        for (vector<KeyFrame *>::iterator vit_prev = vpPreviousNeighbors.begin(), vend_prev = vpPreviousNeighbors.end(); vit_prev != vend_prev; vit_prev++)
        {
            LoopConnections[pKFi].erase(*vit_prev);
        }
        for (vector<KeyFrame *>::iterator vit2 = mvpCurrentConnectedKFs.begin(), vend2 = mvpCurrentConnectedKFs.end(); vit2 != vend2; vit2++)
        {
            LoopConnections[pKFi].erase(*vit2);
        }
    }

    // Optimize graph
    Optimizer::OptimizeEssentialGraph(mpMap, mpMatchedKF, mpCurrentKF, NonCorrectedSim3, CorrectedSim3, LoopConnections, mbFixScale); //TODO 优化本质图

    mpMap->InformNewBigChange();

    // Add loop edge
    mpMatchedKF->AddLoopEdge(mpCurrentKF);
    mpCurrentKF->AddLoopEdge(mpMatchedKF);

    // Launch a new thread to perform Global Bundle Adjustment
    mbRunningGBA = true;
    mbFinishedGBA = false;
    mbStopGBA = false;
    mpThreadGBA = new thread(&LoopClosing::RunGlobalBundleAdjustment, this, mpCurrentKF->mnId);

    // Loop closed. Release Local Mapping.
    mpLocalMapper->Release();

    mLastLoopKFid = mpCurrentKF->mnId;
}

void LoopClosing::SearchAndFuse(const KeyFrameAndPose &CorrectedPosesMap)
{
    ORBmatcher matcher(0.8);

    //TODO 当前帧及其共视关键帧  用更新后的
    for (KeyFrameAndPose::const_iterator mit = CorrectedPosesMap.begin(), mend = CorrectedPosesMap.end(); mit != mend; mit++)
    {
        KeyFrame *pKF = mit->first;

        g2o::Sim3 g2oScw = mit->second;
        cv::Mat cvScw = Converter::toCvMat(g2oScw);

        vector<MapPoint *> vpReplacePoints(mvpLoopMapPoints.size(), static_cast<MapPoint *>(NULL));
        matcher.Fuse(pKF, cvScw, mvpLoopMapPoints, 4, vpReplacePoints);

        // Get Map Mutex
        unique_lock<mutex> lock(mpMap->mMutexMapUpdate);
        const int nLP = mvpLoopMapPoints.size();
        for (int i = 0; i < nLP; i++)
        {
            MapPoint *pRep = vpReplacePoints[i];
            if (pRep)
            {
                pRep->Replace(mvpLoopMapPoints[i]); //TODO 记录要替换的点 完成替换 vpReplacePoints代表的是点本身
            }
        }
    }
}

void LoopClosing::RequestReset()
{
    {
        unique_lock<mutex> lock(mMutexReset);
        mbResetRequested = true;
    }

    while (1)
    {
        {
            unique_lock<mutex> lock2(mMutexReset);
            if (!mbResetRequested)
                break;
        }
        usleep(5000);
    }
}

void LoopClosing::ResetIfRequested()
{
    unique_lock<mutex> lock(mMutexReset);
    if (mbResetRequested)
    {
        mlpLoopKeyFrameQueue.clear();
        mLastLoopKFid = 0;
        mbResetRequested = false;
    }
}

void LoopClosing::RunGlobalBundleAdjustment(unsigned long nLoopKF)
{
    cout << "Starting Global Bundle Adjustment" << endl;

    int idx = mnFullBAIdx;
    Optimizer::GlobalBundleAdjustemnt(mpMap, 10, &mbStopGBA, nLoopKF, false);

    // Update all MapPoints and KeyFrames
    // Local Mapping was active during BA, that means that there might be new keyframes
    // not included in the Global BA and they are not consistent with the updated map.
    // We need to propagate the correction through the spanning tree
    {
        unique_lock<mutex> lock(mMutexGBA);
        if (idx != mnFullBAIdx)
            return;

        if (!mbStopGBA)
        {
            cout << "Global Bundle Adjustment finished" << endl;
            cout << "Updating map ..." << endl;
            mpLocalMapper->RequestStop();
            // Wait until Local Mapping has effectively stopped

            while (!mpLocalMapper->isStopped() && !mpLocalMapper->isFinished())
            {
                usleep(1000);
            }

            // Get Map Mutex
            unique_lock<mutex> lock(mpMap->mMutexMapUpdate);

            // Correct keyframes starting at map first keyframe
            list<KeyFrame *> lpKFtoCheck(mpMap->mvpKeyFrameOrigins.begin(), mpMap->mvpKeyFrameOrigins.end());

            while (!lpKFtoCheck.empty())
            {
                KeyFrame *pKF = lpKFtoCheck.front();
                const set<KeyFrame *> sChilds = pKF->GetChilds();
                cv::Mat Twc = pKF->GetPoseInverse();
                for (set<KeyFrame *>::const_iterator sit = sChilds.begin(); sit != sChilds.end(); sit++)
                {
                    KeyFrame *pChild = *sit;
                    if (pChild->mnBAGlobalForKF != nLoopKF)
                    {
                        cv::Mat Tchildc = pChild->GetPose() * Twc;
                        pChild->mTcwGBA = Tchildc * pKF->mTcwGBA; //*Tcorc*pKF->mTcwGBA;
                        pChild->mnBAGlobalForKF = nLoopKF;
                    }
                    lpKFtoCheck.push_back(pChild);
                }

                pKF->mTcwBefGBA = pKF->GetPose();
                pKF->SetPose(pKF->mTcwGBA);
                lpKFtoCheck.pop_front();
            }

            // Correct MapPoints
            const vector<MapPoint *> vpMPs = mpMap->GetAllMapPoints();

            for (size_t i = 0; i < vpMPs.size(); i++)
            {
                MapPoint *pMP = vpMPs[i];

                if (pMP->isBad())
                    continue;

                if (pMP->mnBAGlobalForKF == nLoopKF)
                {
                    // If optimized by Global BA, just update
                    pMP->SetWorldPos(pMP->mPosGBA);
                }
                else
                {
                    // Update according to the correction of its reference keyframe
                    KeyFrame *pRefKF = pMP->GetReferenceKeyFrame();

                    if (pRefKF->mnBAGlobalForKF != nLoopKF)
                        continue;

                    // Map to non-corrected camera
                    cv::Mat Rcw = pRefKF->mTcwBefGBA.rowRange(0, 3).colRange(0, 3);
                    cv::Mat tcw = pRefKF->mTcwBefGBA.rowRange(0, 3).col(3);
                    cv::Mat Xc = Rcw * pMP->GetWorldPos() + tcw;

                    // Backproject using corrected camera
                    cv::Mat Twc = pRefKF->GetPoseInverse();
                    cv::Mat Rwc = Twc.rowRange(0, 3).colRange(0, 3);
                    cv::Mat twc = Twc.rowRange(0, 3).col(3);

                    pMP->SetWorldPos(Rwc * Xc + twc);
                }
            }

            mpMap->InformNewBigChange();

            mpLocalMapper->Release();

            cout << "Map updated!" << endl;
        }

        mbFinishedGBA = true;
        mbRunningGBA = false;
    }
}

void LoopClosing::RequestFinish()
{
    unique_lock<mutex> lock(mMutexFinish);
    mbFinishRequested = true;
}

bool LoopClosing::CheckFinish()
{
    unique_lock<mutex> lock(mMutexFinish);
    return mbFinishRequested;
}

void LoopClosing::SetFinish()
{
    unique_lock<mutex> lock(mMutexFinish);
    mbFinished = true;
}

bool LoopClosing::isFinished()
{
    unique_lock<mutex> lock(mMutexFinish);
    return mbFinished;
}
} // namespace myslam
