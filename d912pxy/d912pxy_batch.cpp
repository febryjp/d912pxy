/*
MIT License

Copyright(c) 2018-2019 megai2

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files(the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions :

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

*/
#include "stdafx.h"

d912pxy_batch::d912pxy_batch()
{
}


d912pxy_batch::~d912pxy_batch()
{
	delete buffer;

	if (!rawCopyEnabled)
	{
		delete stream;

		copyPSO->Release();
		copyRS->Release();
	}
	else
		PXY_FREE(streamData);
}

void d912pxy_batch::Init()
{
	NonCom_Init(L"draw batch");

	doNewBatch = 0;
	streamIdx = 0;
	batchNum = 0;
	lastBatchCount = 0;
	oddFrame = 0;
	rawCopyEnabled = d912pxy_s.config.GetValueUI32(PXY_CFG_BATCHING_RAW_GPUW);
	forceNewBatch = d912pxy_s.config.GetValueUI32(PXY_CFG_BATCHING_FORCE_NEW_BATCH);
	
	streamOfDlt[0] = 0;

	ZeroMemory(stateTransfer, PXY_BATCH_GPU_DRAW_BUFFER_SIZE);

	if (!rawCopyEnabled)
	{
		stream = new d912pxy_cbuffer(PXY_BATCH_STREAM_SIZE, 1);
		buffer = new d912pxy_cbuffer(PXY_BATCH_GPU_BUFFER_SIZE, 0);

		LOG_INFO_DTDM("GPU delta batching mem usage %u Mb", (PXY_BATCH_GPU_BUFFER_SIZE + PXY_BATCH_STREAM_SIZE) >> 20);

		streamOfDlt[1] = PXY_BATCH_STREAM_PER_FRAME_SIZE;

		intptr_t streamBase = stream->HostPtr();

		streamData = (d912pxy_batch_stream_data_entry*)streamBase;
		streamControl = (d912pxy_batch_stream_control_entry*)(streamBase + PXY_BATCH_STREAM_CONTROL_OFFSET);

		
		memset(mDataDltRef, 0, PXY_BATCH_GPU_ELEMENT_COUNT * 4);

		InitCopyCS();
	}
	else {		
		buffer = new d912pxy_cbuffer(PXY_BATCH_GPU_BUFFER_SIZE * 2, 1);
		PXY_MALLOC(streamData, PXY_BATCH_GPU_BUFFER_SIZE, d912pxy_batch_stream_data_entry*);

		LOG_INFO_DTDM("GPU raw batching mem usage %u Mb", (PXY_BATCH_GPU_BUFFER_SIZE * 3) >> 20);

		streamOfDlt[1] = PXY_BATCH_GPU_BUFFER_SIZE;				

		host_buffer_base_ptr = buffer->HostPtr();
	}
		
	gpu_buffer_base_ptr = buffer->DevPtr();
}

UINT d912pxy_batch::NextBatch()
{
	++batchCount;

	if (forceNewBatch)
		return batchNum++;
	else {
		doNewBatch = 1;
		return batchNum;
	}
}

UINT d912pxy_batch::GetBatchNum()
{
	return batchNum;
}

UINT d912pxy_batch::GetBatchCount()
{
	return batchCount;
}

void d912pxy_batch::SetShaderConstF(UINT type, UINT start, UINT cnt4, float * data)
{
	GPUWrite(data, cnt4, start + ((type != 0) ? PXY_BATCH_GPU_ELEMENT_OFFSET_SHADER_VARS_PIXEL : PXY_BATCH_GPU_ELEMENT_OFFSET_SHADER_VARS_VERTEX));
}

void d912pxy_batch::FrameStart()
{
	doNewBatch = !forceNewBatch;
	topCl = d912pxy_s.dx12.cl->GID(CLG_TOP);

	GPUWriteStart(0, 0);
}

void d912pxy_batch::FrameEnd()
{
	++batchNum;

	if (rawCopyEnabled)
		GPUCpy();
	else
		GPUCSCpy();
	
	lastBatchCount = batchNum-1;
	batchNum = 0;
	batchCount = 0;
}

void d912pxy_batch::GPUCSCpy()
{	
	PIXBeginEvent(topCl, 0xAA00AA, "CSCpy");

	if (!mtCopyEnabled)
	{
		GPUWriteFinalize(0, batchNum);
	}
	else {
		for (int i = 0; i != d912pxy_s.render.replay.GetThreadCount(); ++i)
			GPUWriteFinalize(i, batchNum);
	}

	//megai2: use zero index as skip-unfold point
	streamControl[0].batchNums = 0;

	if (streamIdx & PXY_BATCH_GPU_THREAD_BLOCK_MASK)
	{
		UINT32 npb = (streamIdx & ~PXY_BATCH_GPU_THREAD_BLOCK_MASK) + PXY_BATCH_GPU_THREAD_BLOCK_FIX;

		for (intptr_t i = streamIdx; i != npb; ++i)
		{
			streamControl[i].batchNums = 0;
		}

		streamIdx = npb;
	}

#ifdef ENABLE_METRICS
	d912pxy_s.log.metrics.TrackGPUWDepth(streamIdx);
#endif

	if (streamIdx >= PXY_BATCH_STREAM_ELEMENT_COUNT)
	{
		LOG_ERR_THROW2(-1, L"Too much gpu writes per frame");
	}
		
	UINT ofDlt = streamOfDlt[oddFrame];

	stream->BTransitTo(0, D3D12_RESOURCE_STATE_COPY_DEST, topCl);
	buffer->BTransit(0, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_GENERIC_READ, topCl);
	
	stream->UploadOffsetNB(topCl, ofDlt, streamIdx * PXY_BATCH_STREAM_DATA_SIZE);
	stream->UploadOffsetNB(topCl, ofDlt + PXY_BATCH_STREAM_CONTROL_OFFSET, streamIdx * PXY_BATCH_STREAM_CONTROL_SIZE);	
	topCl->CopyBufferRegion(stream->GetD12Obj(), ofDlt + PXY_BATCH_GPU_ELEMENT_SIZE, buffer->GetD12Obj(), PXY_BATCH_GPU_DRAW_BUFFER_SIZE * lastBatchCount, PXY_BATCH_GPU_TRANSIT_ELEMENTS * PXY_BATCH_GPU_ELEMENT_SIZE);

	buffer->BTransit(0, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE, topCl);
	stream->BTransitTo(0, D3D12_RESOURCE_STATE_GENERIC_READ, topCl);

	topCl->SetPipelineState(copyPSO);
	topCl->SetComputeRootUnorderedAccessView(1, stream->DevPtr() + ofDlt);
	topCl->Dispatch(streamIdx >> PXY_BATCH_GPU_THREAD_BLOCK_SHIFT, 1, 1);

	buffer->BTransit(0, D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, topCl);

	PIXEndEvent(topCl);

	streamIdx = 0;

	oddFrame = !oddFrame;

	intptr_t streamPoint = stream->HostPtr() + streamOfDlt[oddFrame];
	streamData = (d912pxy_batch_stream_data_entry*)streamPoint;
	streamControl = (d912pxy_batch_stream_control_entry*)(streamPoint + PXY_BATCH_STREAM_CONTROL_OFFSET);
}

void d912pxy_batch::GPUCpy()
{
	PIXBeginEvent(topCl, 0xAA00AA, "Cpy");

	UINT ofDlt = streamOfDlt[oddFrame];

	buffer->BTransitTo(0, D3D12_RESOURCE_STATE_COPY_DEST, topCl);
	buffer->UploadOffsetNB(topCl, ofDlt, batchNum*PXY_BATCH_GPU_DRAW_BUFFER_SIZE);
	buffer->BTransitTo(0, D3D12_RESOURCE_STATE_GENERIC_READ, topCl);
	
	PIXEndEvent(topCl);

	streamIdx = 0;

	oddFrame = !oddFrame;

	gpu_buffer_base_ptr = buffer->DevPtr() + streamOfDlt[oddFrame];
	host_buffer_base_ptr = buffer->HostPtr() + streamOfDlt[oddFrame];
}

void d912pxy_batch::GPUWriteFinalize(UINT tid, UINT endBatch)
{
	if (rawCopyEnabled)
		return;

	if (mtCopyEnabled)
	{
		UINT dltRefOfs = tid * PXY_BATCH_GPU_ELEMENT_COUNT;

		if (tid == (d912pxy_s.render.replay.GetThreadCount()-1))
		{
			for (int i = 0; i != PXY_BATCH_GPU_ELEMENT_COUNT; ++i)
				streamControl[mDataDltRefMT[dltRefOfs + i]].endBatch = endBatch;

			return;
		}

		UINT dltRefOfsNext = (tid+1) * PXY_BATCH_GPU_ELEMENT_COUNT;
		
		for (int i = 0; i != PXY_BATCH_GPU_ELEMENT_COUNT; ++i)
		{
			//megai2: if next thread writed to this element, we know where current thread write ends
			//otherwise transfer this write to next thread
			if (mDataDltRefMT[dltRefOfsNext + i] != -1)
				streamControl[mDataDltRefMT[dltRefOfs + i]].endBatch = mDataDltRefMTTransfer[dltRefOfsNext + i];
			else
				mDataDltRefMT[dltRefOfsNext + i] = mDataDltRefMT[dltRefOfs + i];
		}

	}
	else 
		for (int i = 0; i != PXY_BATCH_GPU_ELEMENT_COUNT; ++i)
			streamControl[mDataDltRef[i]].endBatch = endBatch;
}

void d912pxy_batch::GPUWriteStart(UINT tid, UINT fromReplay)
{	
	if (rawCopyEnabled)
		return;

	if (!fromReplay)
	{
		topCl->SetComputeRootSignature(copyRS);
		topCl->SetComputeRootUnorderedAccessView(0, buffer->DevPtr());

		if (tid == 0)
			streamIdx += PXY_BATCH_GPU_TRANSIT_ELEMENTS + 1;	

		return;
	}
	
	if (mtCopyEnabled)
	{
		if (!tid)
		{
			memset(mDataDltRefMT, 0, PXY_BATCH_GPU_ELEMENT_COUNT * 4);			
			GPUWriteControlMT(1, 0, PXY_BATCH_GPU_TRANSIT_ELEMENTS, 0, 0);
			return;
		}

		UINT dltRefOfs = tid*PXY_BATCH_GPU_ELEMENT_COUNT;

		for (int i = 0; i != PXY_BATCH_GPU_ELEMENT_COUNT; ++i)
		{
			mDataDltRefMT[dltRefOfs + i] = -1;
			mDataDltRefMTTransfer[dltRefOfs + i] = 0;
		}
	}
	else {
		memset(mDataDltRef, 0, PXY_BATCH_GPU_ELEMENT_COUNT * 4);		
		GPUWriteControl(1, 0, PXY_BATCH_GPU_TRANSIT_ELEMENTS, 0);
	}		
}

void d912pxy_batch::PreDIPRaw(ID3D12GraphicsCommandList * cl, UINT bid)
{
	UINT64 bid_offset = PXY_BATCH_GPU_DRAW_BUFFER_SIZE * bid;

	memcpy((void*)(host_buffer_base_ptr + bid_offset), stateTransfer, PXY_BATCH_GPU_DRAW_BUFFER_SIZE);

	cl->SetGraphicsRootConstantBufferView(3, gpu_buffer_base_ptr + bid_offset);
}

void d912pxy_batch::PreDIP(ID3D12GraphicsCommandList* cl, UINT bid)
{
	cl->SetGraphicsRootConstantBufferView(3, gpu_buffer_base_ptr + PXY_BATCH_GPU_DRAW_BUFFER_SIZE * bid);
}

void d912pxy_batch::ClearShaderVars()
{
	SetShaderConstF(0, 0, 256, (float*)stateTransfer);
	SetShaderConstF(1, 0, 256, (float*)stateTransfer);
}

void d912pxy_batch::GPUWrite(void * src, UINT size, UINT offset)
{
	if (doNewBatch)
	{
		doNewBatch = 0;
		++batchNum;
	}

	memcpy(&streamData[streamIdx], src, size << 4);
	
	//43-59 ss
	//125 mt
	//306 ps
	d912pxy_s.render.replay.GPUW(streamIdx, offset, size, batchNum);
	//GPUWriteControl(streamIdx, offset, size, batchNum);	

	streamIdx += size;


}

void d912pxy_batch::GPUWriteRaw(UINT64 si, UINT64 of, UINT64 cnt, UINT64 bn)
{
	memcpy(&stateTransfer[of << 4], &streamData[si], cnt << 4);
}

void d912pxy_batch::GPUWriteControl(UINT64 si, UINT64 of, UINT64 cnt, UINT64 bn)
{
	UINT64 i = of;
	while (i != (of + cnt))
	{
		d912pxy_batch_stream_control_entry* ctl = &streamControl[si];

		ctl->dstOffset = (UINT32)i;
		ctl->startBatch = (UINT32)bn;

		streamControl[mDataDltRef[i]].endBatch = (UINT32)bn;
		mDataDltRef[i] = (UINT32)si;

		++si;
		++i;
	}
}

void d912pxy_batch::GPUWriteControlMT(UINT64 si, UINT64 of, UINT64 cnt, UINT64 bn, UINT tid)
{
	UINT32 tidOffset = tid * PXY_BATCH_GPU_ELEMENT_COUNT;

	UINT64 i = of;
	while (i != (of + cnt))
	{
		d912pxy_batch_stream_control_entry* ctl = &streamControl[si];

		ctl->dstOffset = (UINT32)i;
		ctl->startBatch = (UINT32)bn;

		UINT32 dltRef = mDataDltRefMT[tidOffset + i];
		if (dltRef != -1)
			streamControl[dltRef].endBatch = (UINT32)bn;
		else
			mDataDltRefMTTransfer[tidOffset + i] = (UINT32)bn;

		mDataDltRefMT[tidOffset+i] = (UINT32)si;

		++si;
		++i;
	}
}

void d912pxy_batch::InitCopyCS()
{

	//copy cs Root signature
	D3D12_ROOT_PARAMETER rootParameters[2] = {
		{ D3D12_ROOT_PARAMETER_TYPE_UAV, {0}, D3D12_SHADER_VISIBILITY_ALL},
		{ D3D12_ROOT_PARAMETER_TYPE_UAV, {0}, D3D12_SHADER_VISIBILITY_ALL}
	};

	rootParameters[0].Descriptor = { 0, 0 };
	rootParameters[1].Descriptor = { 1, 0 };

	D3D12_ROOT_SIGNATURE_DESC rootSignatureDesc = { 2, rootParameters, 0, 0, D3D12_ROOT_SIGNATURE_FLAG_NONE };

	copyRS = d912pxy_s.dev.ConstructRootSignature(&rootSignatureDesc);

	mtCopyEnabled = (d912pxy_s.config.GetValueUI64(PXY_CFG_REPLAY_THREADS) != 1);
	
	//copy cs hlsl code
	d912pxy_shader_replacer* CScodec = new d912pxy_shader_replacer(0, 0, 3, 0);
	d912pxy_shader_code CScode = CScodec->GetCodeCS();
	delete CScodec;

	//copy cs PSO
	D3D12_COMPUTE_PIPELINE_STATE_DESC dsc = { 
		copyRS, 
		{CScode.code, CScode.sz}, 
		0, 
		{NULL, 0}, 
		D3D12_PIPELINE_STATE_FLAG_NONE 
	};

	LOG_ERR_THROW2(d912pxy_s.dx12.dev->CreateComputePipelineState(&dsc, IID_PPV_ARGS(&copyPSO)), "CS pso creation err");

	//cleanup
	if (!CScode.blob)
		PXY_FREE(CScode.code);
}
