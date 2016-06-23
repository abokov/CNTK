#pragma once

// the header files located in Source\Multiverso\include
#include <multiverso/multiverso.h>
#include <multiverso/util/log.h>
#include <multiverso/util/configure.h>

#include <multiverso/table/matrix.h>
#include <multiverso/updater/updater.h>

#pragma comment(lib, "Multiverso.lib")

#ifndef CPUONLY
#include <cuda_runtime.h>
#pragma comment (lib, "cudart.lib")     // for cudaMemcpyAsync()
#endif

// TODO: test for the model aggregation
#include "MPIWrapper.h"
#include "ComputationNetwork.h"
#include "TimerUtility.h"

#include <functional>
#include <thread>
#include <unordered_map>
#include <numeric>
#include <algorithm>

namespace Microsoft { namespace MSR { namespace CNTK {
#define MULTIVERSO_DEBUG

#ifndef CPUONLY
#define CudaErrorCheck(ans) { gpuAssert((ans), __FILE__, __LINE__); }
inline void gpuAssert(cudaError_t code, const char *file, int line, bool abort = true)
{
    if (code != cudaSuccess)
    {
        fprintf(stderr, "GPUassert: %s %s %d\n", cudaGetErrorString(code), file, line);
        if (abort) exit(code);
    }
}
#endif

enum class AdjustLearningRateatBeginning : int
{
    None = 0,
    Linearly = 1,
    Staircase = (1 << 1),
};

template<class ElemType = float>
class MultiversoHelper
{
typedef shared_ptr<ComputationNode<ElemType>> ComputationNodePtr;
public:
MultiversoHelper(const std::list<ComputationNodeBasePtr> & learnableNodes,
    int MPINodeNum,
    bool isAsyncBuffered = true,
    bool isSimulatingMA = false,
    AdjustLearningRateatBeginning adjusttype = AdjustLearningRateatBeginning::None,
    double adjustcoef = 0.2,
    size_t adjustnbmb = 600,
    int traceLevel = 0,
    int syncPerfStats = 0,
    const MPIWrapperPtr& pMPI = nullptr)
    : m_modelSyncCount(0), m_adjustLearningRateAtBeginningType(adjusttype),
    m_adjustCoefficient(adjustcoef), m_adjustMBNumber(adjustnbmb),
    m_totalClientNumber(MPINodeNum), m_isUseAsyncBuffered(isAsyncBuffered),
    m_traceLevel(traceLevel), m_isAverage(isSimulatingMA), m_isSycned(false),
    m_pMPI(pMPI), m_syncPerfStats(syncPerfStats)
{
    if (m_isAverage)
    {
        m_isSycned = true;
        m_isUseAsyncBuffered = false;
    }
    //Pipeline releated variables
    m_localCacheNumber = m_isUseAsyncBuffered ? 2 : 1;
    m_cacheSwapIndex = new int[m_localCacheNumber];

    //CPU asynchronous buffer
    m_cpuAsyncBuffer = new ElemType*[m_localCacheNumber];

    //Get option used by multiverso sparse update

    m_getOptions.reserve(m_localCacheNumber);
    m_addOptions.reserve(m_localCacheNumber);
#ifndef CPUONLY
    //GPU asynchronous buffer
    m_gpuAsyncBuffer.resize(m_localCacheNumber);
    //creat an communication stream for the data tranfer between GPU and CPU
    CudaErrorCheck(cudaStreamCreate(&_commStream));
#endif
    m_bufferInUse = 0;
    for (int i = 0; i < m_localCacheNumber; i++)
        m_cacheSwapIndex[i] = (i + 1) % m_localCacheNumber;

    m_prefetchThread = nullptr;

    if (m_traceLevel > 5)
        multiverso::Log::ResetLogLevel(multiverso::LogLevel::Debug);
    else if (m_traceLevel > 4)
        multiverso::Log::ResetLogLevel(multiverso::LogLevel::Error);

    if (m_isSycned)
        multiverso::SetCMDFlag("sync", true);

    MultiversoInit(learnableNodes);
}

~MultiversoHelper()
{
    fprintf(stderr, "~MultiversoHelper\n");
    fflush(stderr);

    if (m_isUseAsyncBuffered && m_prefetchThread != nullptr && m_prefetchThread->joinable())
        m_prefetchThread->join();

    delete m_cacheSwapIndex, m_deltaArray;

    for (size_t i = 0; i < m_localCacheNumber; i++)
    {
#ifndef CPUONLY
        CudaErrorCheck(cudaFreeHost(m_cpuAsyncBuffer[i]));
#else
        delete m_cpuAsyncBuffer[i];
#endif
    }
    delete m_cpuAsyncBuffer;
#ifndef CPUONLY
    CudaErrorCheck(cudaStreamDestroy(_commStream));
#endif
    multiverso::MV_ShutDown(false);
}

// upoload preCompute model to the parameter servers
void InitModel(const std::list<ComputationNodeBasePtr> & learnableNodes)
{
    float factor = 1.0f / m_totalClientNumber;

    int i = 0; // indicate the index of learnable nodes
    for (auto nodeIter = learnableNodes.begin(); nodeIter != learnableNodes.end(); nodeIter++, i++)
    {
        ComputationNodePtr node = dynamic_pointer_cast<ComputationNode<ElemType>>(*nodeIter);
        Matrix<ElemType> &mat = node->Value();

#ifndef CPUONLY
        for (int j = 0; j < m_localCacheNumber; j++)
            m_gpuAsyncBuffer[j].push_back(mat.DeepClone());
#endif
        ElemType* px = m_cpuAsyncBuffer[0] + m_tableOffsets[i];
        mat.CopyToArray(px, m_tableLength[i]);
    }

    for (int i = 1; i < m_localCacheNumber; i++)
        memcpy(m_cpuAsyncBuffer[i], m_cpuAsyncBuffer[0], sizeof(ElemType) * m_totalModelSize);

    memcpy(m_deltaArray, m_cpuAsyncBuffer[0], sizeof(ElemType) * m_totalModelSize);

    // because the parameter server will minus the delta on the server, so that we should send the minus initial model to the server.
    std::transform(m_deltaArray, m_deltaArray + m_totalModelSize, m_deltaArray, std::bind1st(std::multiplies<ElemType>(), -factor));

    for (int widx = 0; widx < m_tableCount; widx++)
    {
        if (m_isSparseArray[widx])
        {
            auto multiversoMatrix = m_matrixMap->at(widx);
            ElemType* px = m_deltaArray + m_tableOffsets[widx];
            multiversoMatrix->Add(px, m_tableLength[widx], m_addOptions[0]);
            multiversoMatrix->Get(px, m_tableLength[widx], m_getOptions[0]);
            WaitAll();
            multiversoMatrix->Get(px, m_tableLength[widx], m_getOptions[0]);
        }
        else
        {
            auto multiversoMatrix = m_matrixMap->at(widx);
            ElemType* px = m_deltaArray + m_tableOffsets[widx];
            multiversoMatrix->Add(px, m_tableLength[widx]);
            multiversoMatrix->Get(px, m_tableLength[widx]);
            WaitAll();
            multiversoMatrix->Get(px, m_tableLength[widx]);
        }
    }

    if (std::equal(m_deltaArray, m_deltaArray + m_totalModelSize, m_cpuAsyncBuffer[0]))
        multiverso::Log::Info("multiverso initial model loaded.\n");
    m_reportTimer.Start();
}

//ASGD logic
bool PushAndPullModel(const std::list<ComputationNodeBasePtr> & learnableNodes, size_t sampleSinceLastSynced = 0)
{
    //Note: maybe overflow.
    m_modelSyncCount++;

    Timer timer;
    timer.Restart();
    WaitAsyncBuffer();
    timer.Stop();


    m_bufferInUse = m_cacheSwapIndex[m_bufferInUse];

    int i = 0; // indicate the index of learnable nodes
    if (m_isUseAsyncBuffered)
    {
        timer.Restart();
        for (auto nodeIter = learnableNodes.begin(); nodeIter != learnableNodes.end(); nodeIter++, i++)
        {
            ComputationNodePtr node = dynamic_pointer_cast<ComputationNode<ElemType>>(*nodeIter);
            Microsoft::MSR::CNTK::Matrix<ElemType> &mat = node->Value();
#ifndef CPUONLY
            //CNTK model -> GPU buffer
            CudaErrorCheck(cudaMemcpy(m_gpuAsyncBuffer[m_bufferInUse][i].Data(),
                mat.Data(),
                mat.GetNumElements() * sizeof(ElemType),
                cudaMemcpyDeviceToDevice));

            //GPU buffer -> CNTK model
            CudaErrorCheck(cudaMemcpy(mat.Data(),
                m_gpuAsyncBuffer[m_cacheSwapIndex[m_bufferInUse]][i].Data(),
                mat.GetNumElements() * sizeof(ElemType),
                cudaMemcpyDeviceToDevice));
#else
            ElemType * px = m_cpuAsyncBuffer[m_bufferInUse] + m_tableOffsets[i];

            mat.CopyToArray(px, m_tableLength[i]);

            ElemType * py = m_cpuAsyncBuffer[m_cacheSwapIndex[m_bufferInUse]] + m_tableOffsets[i];

            mat.SetValue(mat.GetNumRows(), mat.GetNumCols(), mat.GetDeviceId(), py);


            delete px;
#endif
        }
        timer.Stop();
        if (m_traceLevel > 3)
        {
            double time = timer.ElapsedSeconds();
            fprintf(stderr, "\t\t -- pullAndRequest, GPU -> GPU time %lf \n", time);
        }
#ifndef CPUONLY
        m_prefetchThread = new thread([&](){
            float factor = DecayCoefficient();
            int t_cacheIdx = m_bufferInUse;
            int deviceId = m_gpuAsyncBuffer[t_cacheIdx][0].GetDeviceId();

            CudaErrorCheck(cudaSetDevice(deviceId));

            Timer threadTimer;
            threadTimer.Restart();
            for (int widx = 0; widx < m_tableCount; widx++)
            {
                ElemType * px = m_deltaArray + m_tableOffsets[widx];
                //GPU buffer -> CPU buffer
                CudaErrorCheck(cudaMemcpyAsync(px,
                    m_gpuAsyncBuffer[t_cacheIdx][widx].Data(),
                    m_gpuAsyncBuffer[t_cacheIdx][widx].GetNumElements() * sizeof(ElemType),
                    cudaMemcpyDeviceToHost,
                    _commStream));
            }
            // waiting copy from GPU to CPU has finished
            CudaErrorCheck(cudaStreamSynchronize(_commStream));
            threadTimer.Stop();
            if (m_traceLevel > 3)
            {
                double time = threadTimer.ElapsedSeconds();
                fprintf(stderr, "\t\t -- pullAndRequest, GPU -> CPU time %lf \n", time);
            }

            // delta =  gradient * learning_rate
            std::transform(m_cpuAsyncBuffer[t_cacheIdx], m_cpuAsyncBuffer[t_cacheIdx] + m_totalModelSize, m_deltaArray, m_deltaArray, std::minus<ElemType>());

            threadTimer.Restart();
            // lr decay
            std::transform(m_deltaArray, m_deltaArray + m_totalModelSize, m_deltaArray, std::bind1st(std::multiplies<ElemType>(), factor));

            for (int widx = 0; widx < m_tableCount; widx++)
            {
                if (m_isSparseArray[widx])
                {
                    auto multiversoMatrix = m_matrixMap->at(widx);
                    ElemType* px = m_deltaArray + m_tableOffsets[widx];
                    ElemType* py = m_cpuAsyncBuffer[t_cacheIdx] + m_tableOffsets[widx];
                    multiversoMatrix->Add(px, m_tableLength[widx], m_addOptions[t_cacheIdx]);
                    multiversoMatrix->Get(py, m_tableLength[widx], m_getOptions[t_cacheIdx]);
                }
                else
                {
                    auto multiversoMatrix = m_matrixMap->at(widx);
                    ElemType* px = m_deltaArray + m_tableOffsets[widx];
                    ElemType* py = m_cpuAsyncBuffer[t_cacheIdx] + m_tableOffsets[widx];
                    multiversoMatrix->Add(px, m_tableLength[widx]);
                    multiversoMatrix->Get(py, m_tableLength[widx]);
                }
            }
            threadTimer.Stop();
            if (m_traceLevel > 3)
            {
                double time = threadTimer.ElapsedSeconds();
                fprintf(stderr, "\t\t -- pullAndRequest, Worker <--> Multiverso time %lf \n", time);
            }

            threadTimer.Restart();
            // copy parameters from CPU buffer to GPU buffer
            for (int widx = 0; widx < m_tableCount; widx++)
            {
                ElemType * py = m_cpuAsyncBuffer[t_cacheIdx] + m_tableOffsets[widx];

                CudaErrorCheck(cudaMemcpyAsync(m_gpuAsyncBuffer[t_cacheIdx][widx].Data(),
                    py,
                    m_gpuAsyncBuffer[t_cacheIdx][widx].GetNumElements() * sizeof(ElemType),
                    cudaMemcpyHostToDevice,
                    _commStream));
            }
            CudaErrorCheck(cudaStreamSynchronize(_commStream));
            threadTimer.Stop();
            if (m_traceLevel > 3)
            {
                double time = threadTimer.ElapsedSeconds();
                fprintf(stderr, "\t\t -- pullAndRequest, CPU -> GPU time %lf \n", time);
            }
        });
#else
        m_prefetchThread = new thread([&](){
            float factor = DecayCoefficient();
            int t_cacheIdx = m_bufferInUse;

            std::transform(m_cpuAsyncBuffer[t_cacheIdx], m_cpuAsyncBuffer[t_cacheIdx] + m_totalModelSize, m_deltaArray, m_deltaArray, std::minus<ElemType>());
            std::transform(m_deltaArray, m_deltaArray + m_totalModelSize, m_deltaArray, std::bind1st(std::multiplies<ElemType>(), factor));
            for (int widx = 0; widx < m_tableCount; widx++)
            {
                auto multiversoMatrix = m_matrixMap->at(widx);
                ElemType* px = m_deltaArray + m_tableOffsets[widx];
                ElemType* py = m_cpuAsyncBuffer[t_cacheIdx] + m_tableOffsets[widx];
                multiversoMatrix->Add(px, m_tableLength[widx], m_addOptions[t_cacheIdx]);
                multiversoMatrix->Get(py, m_tableLength[widx], m_getOptions[t_cacheIdx]);
            }

        });
#endif
    }
    else
    {
        timer.Restart();
        float factor = DecayCoefficient();
        i = 0;
        for (auto nodeIter = learnableNodes.begin(); nodeIter != learnableNodes.end(); nodeIter++, i++)
        {
            ComputationNodePtr node = dynamic_pointer_cast<ComputationNode<ElemType>>(*nodeIter);
            Microsoft::MSR::CNTK::Matrix<ElemType> &mat = node->Value();

            ElemType * px = m_deltaArray + m_tableOffsets[i];
            mat.CopyToArray(px, m_tableLength[i]);
        }

        timer.Stop();
        if (m_traceLevel > 3)
        {
            double time = timer.ElapsedSeconds();
            fprintf(stderr, "\t\t -- pullAndRequest, GPU -> CPU time %lf \n", time);
        }
        std::transform(m_cpuAsyncBuffer[0], m_cpuAsyncBuffer[0] + m_totalModelSize, m_deltaArray, m_deltaArray, std::minus<ElemType>());

        // lr decay
        if (m_isAverage)
        {
            factor = ModelAggregationCoefficient(sampleSinceLastSynced);
            std::transform(m_deltaArray, m_deltaArray + m_totalModelSize, m_deltaArray, std::bind1st(std::multiplies<ElemType>(), factor));
            if (m_traceLevel > 2 && m_syncPerfStats != 0)
            {
                if (m_modelSyncCount % m_syncPerfStats == 0)
                    ReportPerfStats(m_totalClientNumber * m_sampleSinceLastReport, m_sampleSinceLastReport);
                else
                    m_sampleSinceLastReport += sampleSinceLastSynced;
            }

        }
        else
        {
            std::transform(m_deltaArray, m_deltaArray + m_totalModelSize, m_deltaArray, std::bind1st(std::multiplies<ElemType>(), factor));
        }
        timer.Restart();
        for (int widx = 0; widx < m_tableCount; widx++)
        {
            if (m_isSparseArray[widx])
            {
                auto multiversoMatrix = m_matrixMap->at(widx);
                ElemType* px = m_deltaArray + m_tableOffsets[widx];
                ElemType* py = m_cpuAsyncBuffer[0] + m_tableOffsets[widx];
                multiversoMatrix->Add(px, m_tableLength[widx], m_addOptions[0]);
                multiversoMatrix->Get(py, m_tableLength[widx], m_getOptions[0]);
            }
            else
            {
                auto multiversoMatrix = m_matrixMap->at(widx);
                ElemType* px = m_deltaArray + m_tableOffsets[widx];
                ElemType* py = m_cpuAsyncBuffer[0] + m_tableOffsets[widx];
                multiversoMatrix->Add(px, m_tableLength[widx]);
                multiversoMatrix->Get(py, m_tableLength[widx]);
            }
        }
        timer.Stop();
        if (m_traceLevel > 3)
        {
            double time = timer.ElapsedSeconds();
            fprintf(stderr, "\t\t -- pullAndRequest, Worker <--> Multiverso time %lf \n", time);
        }
        timer.Restart();
        i = 0;
        for (auto nodeIter = learnableNodes.begin(); nodeIter != learnableNodes.end(); nodeIter++, i++)
        {
            ComputationNodePtr node = dynamic_pointer_cast<ComputationNode<ElemType>>(*nodeIter);
            Microsoft::MSR::CNTK::Matrix<ElemType> &mat = node->Value();

            ElemType * px = m_cpuAsyncBuffer[0] + m_tableOffsets[i];
            mat.SetValue(mat.GetNumRows(), mat.GetNumCols(), mat.GetDeviceId(), px);
        }
        timer.Stop();
        if (m_traceLevel > 3)
        {
            double time = timer.ElapsedSeconds();
            fprintf(stderr, "\t\t -- pullAndRequest, CPU -> GPU time %lf \n", time);
        }
    }
    return true;
}

void PushModel(const std::list<ComputationNodeBasePtr> & learnableNode)
{

}

void PullModel(const std::list<ComputationNodeBasePtr> & learnableNode)
{

}

void WaitAll()
{
    multiverso::MV_Barrier();
}

void WaitAsyncBuffer()
{
    if (m_prefetchThread != nullptr && m_prefetchThread->joinable())
    {
        m_prefetchThread->join();
        delete m_prefetchThread;
        m_prefetchThread = nullptr;
    }
}

private:
    void MultiversoInit(const std::list<ComputationNodeBasePtr> & learnableNodes)
    {
        assert(!m_isInitialized);
        m_isInitialized = true;

        multiverso::SetCMDFlag<std::string>(std::string("updater_type"), std::string("sgd"));
        multiverso::MV_Init();


        for (int i = 0; i < m_localCacheNumber; i++)
        {
            m_getOptions.push_back(new multiverso::GetOption());
            m_getOptions.at(i)->set_worker_id(m_localCacheNumber * multiverso::MV_WorkerId() + i);
            m_addOptions.push_back(new multiverso::AddOption());
            m_addOptions.at(i)->set_worker_id(m_localCacheNumber * multiverso::MV_WorkerId() + i);
        }

        m_matrixMap = new std::vector< multiverso::MatrixWorker<ElemType>*>();
        m_serverMap = new std::vector< multiverso::MatrixServer<ElemType>*>();

        //weights
        std::wstring sparse_tag{ L"Sparse" };
        int i = 0;
        for (auto nodeIter = learnableNodes.begin(); nodeIter != learnableNodes.end(); nodeIter++, i++)
        {
            ComputationNodePtr node = dynamic_pointer_cast<ComputationNode<ElemType>>(*nodeIter);
            Matrix<ElemType> &mat = node->Value();
            size_t layerSize = mat.GetNumElements();
            size_t layerRowSize = mat.GetNumRows();
            size_t layerColSize = mat.GetNumCols();
            std::wstring nodeName = node->NodeName();
            auto found = nodeName.find(sparse_tag);
            m_isSparseArray.push_back(false);

            fprintf(stderr, "Layer %ls, size: %d, row size: %d, col size: %d.\n", node->NodeName().c_str(), (int)layerSize, (int)layerRowSize, (int)layerColSize);
            fflush(stderr);
            if (found != std::string::npos)
            {
                m_isSparseArray[i] = true;
                fprintf(stderr, "Layer %ls using sparseMatrix. row size: %d, col size: %d\n", nodeName.c_str(), (int)layerColSize, (int)layerRowSize);
                fflush(stderr);
                m_matrixMap->push_back(new multiverso::MatrixWorker<ElemType>(layerColSize, layerRowSize, true));
                m_serverMap->push_back(new multiverso::MatrixServer<ElemType>(layerColSize, layerRowSize, true, m_isUseAsyncBuffered));

            }
            else
            {
                m_isSparseArray[i] = false;
                m_matrixMap->push_back(new multiverso::MatrixWorker<ElemType>(layerRowSize, layerColSize, false));
                m_serverMap->push_back(new multiverso::MatrixServer<ElemType>(layerRowSize, layerColSize, false, m_isUseAsyncBuffered));
            }

            m_tableLength.push_back(layerSize);
        }

        m_tableCount = m_tableLength.size();

        // cacluate total of learnable node's size
        m_totalModelSize = accumulate(m_tableLength.begin(), m_tableLength.end(), 0);

        multiverso::MV_Barrier();

        size_t idx = 0;
        for (size_t len : m_tableLength)
        {
            m_tableOffsets.push_back(idx);
            idx += len;
        }

#ifndef CPUONLY
        for (int i = 0; i < m_localCacheNumber; i++)
            m_gpuAsyncBuffer[i].reserve(m_tableCount);

        //create pinned memory
        for (int i = 0; i < m_localCacheNumber; ++i)
            CudaErrorCheck(cudaMallocHost((void **)&m_cpuAsyncBuffer[i], sizeof(ElemType) * (m_totalModelSize), cudaHostAllocPortable));

        CudaErrorCheck(cudaMallocHost((void **)&m_deltaArray, sizeof(ElemType) * (m_totalModelSize), cudaHostAllocPortable));
#else
        for (int i = 0; i < m_localCacheNumber; i++)
            m_cpuAsyncBuffer[i] = new ElemType[m_totalModelSize];
#endif
    }

    float DecayCoefficient()
    {
        float f = 1.f;
        switch (m_adjustLearningRateAtBeginningType)
        {
        case AdjustLearningRateatBeginning::None:
            break;
        case AdjustLearningRateatBeginning::Linearly:
            f = min(f, max(0.f, (float)(m_adjustCoefficient + (1 - m_adjustCoefficient) / m_adjustMBNumber * m_modelSyncCount)));
            break;
        case AdjustLearningRateatBeginning::Staircase:
            f = min(f, max(0.f, (float)(m_adjustCoefficient * (m_modelSyncCount / m_adjustMBNumber + 1))));
            break;
        default:
            break;
        }
        return f;
    }

    float ModelAggregationCoefficient(size_t samplesSinceLastSync)
    {
        float factor = 0;
        int   nTotalSamples = samplesSinceLastSync;
        // TODO[qiwye] will conflict with multiverso
        //m_pMPI->AllReduce(&nTotalSamples, 1);

        if (nTotalSamples <= 0)
        {
            factor = 1.0f / m_pMPI->NumNodesInUse();
            // give an estimated one 
        }
        else
        {
            factor = (samplesSinceLastSync + 0.0f) / nTotalSamples;
        }
        factor = 1.0f / m_pMPI->NumNodesInUse();
        return factor;
    }

    inline void transpose(ElemType *src, ElemType *dst, const int N, const int M)
    {
        for (auto n = 0; n < N*M; n++) {
            auto i = n / N;
            auto j = n%N;
            dst[n] = src[M*j + i];
        }
    }

    void ReportPerfStats(size_t totalSamplesProcessedSinceLastReport,
                           size_t localSamplesProcessedSinceLastReport)
    {
        m_reportTimer.Stop();
        double secondsSinceLastReport = m_reportTimer.ElapsedSeconds();
        m_reportTimer.Restart();

        float totalThroughput = secondsSinceLastReport > 0 ? (float)totalSamplesProcessedSinceLastReport / ((float)secondsSinceLastReport * 1000.0f) : 0.0f;
        float throughputPerWorker = totalThroughput / m_totalClientNumber;

        string prefix = "\t\t(sim-model aggregation stats) %d-th sync: %8.2f seconds since last report ; %d samples processed by %d workers (%d by me);\n"
            "\t\t(sim-model aggregation stats) %d-th sync: totalThroughput = %.2fk samplesPerSecond , throughputPerWorker = %.2fk samplesPerSecond\n";
        fprintf(stderr, prefix.c_str(), (int)m_modelSyncCount, secondsSinceLastReport, (int)totalSamplesProcessedSinceLastReport, (int)m_totalClientNumber, (int)localSamplesProcessedSinceLastReport,
            (int)m_modelSyncCount, totalThroughput, throughputPerWorker);
        m_sampleSinceLastReport = 0;

    }
    std::vector<multiverso::MatrixWorker<ElemType>*>* m_matrixMap;
    std::vector<multiverso::MatrixServer<ElemType>*>* m_serverMap;
    std::vector<bool> m_isSparseArray;

    thread * m_prefetchThread;
    bool m_isInitialized;
    bool m_isSycned;
    bool m_isAverage;

    int m_totalClientNumber;
    int m_traceLevel;
    int m_syncPerfStats;
    Timer m_reportTimer;
    size_t m_modelSyncCount;
    size_t m_sampleSinceLastReport;

    bool m_isUseAsyncBuffered;
    int m_localCacheNumber;
    int * m_cacheSwapIndex;
    int m_bufferInUse;
    std::vector< multiverso::GetOption*> m_getOptions; // used by sparse table
    std::vector< multiverso::AddOption*> m_addOptions; // used by sparse table


    AdjustLearningRateatBeginning m_adjustLearningRateAtBeginningType;
    double m_adjustCoefficient;
    size_t m_adjustMBNumber;

    vector<size_t> m_tableLength;
    size_t m_totalModelSize;
    vector<size_t> m_tableOffsets;
    ElemType * m_deltaArray;
    ElemType ** m_cpuAsyncBuffer;

    MPIWrapperPtr m_pMPI;

    //GPU double buffer
    std::vector<std::vector<Matrix<ElemType>   >> m_gpuAsyncBuffer;
    int m_tableCount;

#ifndef CPUONLY
    cudaStream_t _commStream;
#endif
};
}  // namespace CNTK
}  // namespace MSR
}  // namespace Microsoft
