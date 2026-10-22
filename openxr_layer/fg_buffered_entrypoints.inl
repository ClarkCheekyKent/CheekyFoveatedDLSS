// Graphics entry points used by the layer capture hooks and GPU fixtures.
// Included inside BufferedPresenter. The pacing loops use FrameBackend instead.
    std::recursive_mutex* dispatch_mutex11() const noexcept override;
    bool accept_guide_queue(const CheekyFgPair& guides,FrameGuides12& payload,bool hookless) noexcept;
    bool setup(XrSession s,const XrFns& f,ID3D12Device* dev,ID3D12CommandQueue* q) noexcept override;
    bool setup11(XrSession s,const XrFns& f,ID3D11Device* dev,std::uint64_t generation,std::recursive_mutex* dispatch) noexcept;
    bool capture11(XrSwapchain sc,unsigned index,ID3D11Texture2D* source,unsigned width,unsigned height,unsigned array,DXGI_FORMAT declared) override;
    bool capture_composed11(XrSwapchain sc,ID3D11Texture2D* source,DXGI_FORMAT format);
    bool prepare_native11(Native& job,const XrCompositionLayerProjection* projection,bool inference_slot);
    CheekyFgOverhead overhead() const noexcept override;
    CheekyFgExperimentDiagnostics experiments() const noexcept override;
    void measure_worker_memory();
    void measure_presenter_memory();
    void collect_color_stamps();
    void destroy_private_chain(XrSwapchain chain);
    void destroy_chains() noexcept;
    void reset() noexcept override;
    void reset_gpu_metrics();
    bool capture_quad_planes(const XrCompositionLayerProjection& raw,const XrCompositionLayerProjection& logical,
        const std::array<std::array<float,4>,2>& insets,const CheekyFgPair& pair);
    bool capture(XrSwapchain sc,unsigned,ID3D12Resource* original,unsigned,unsigned,unsigned,
                 DXGI_FORMAT declared=DXGI_FORMAT_UNKNOWN) noexcept override;
    bool ensure_chain(Chain& chain,const Image& image,bool scene=false);
    bool upload11(Chain& chain,const Image& source);
    bool upload_image(Chain& chain,const Image& source,bool scene=false);
    bool upload_batch(const std::vector<std::pair<Chain*,Image>>& requests);
    void note_direct_failure_locked(CheekyFgDirectReason reason,unsigned eye=0,const DirectRequest* request=nullptr,
    const Chain* chain=nullptr,const D3D12_RESOURCE_DESC* image=nullptr,std::int32_t api_result=0);
    bool service_direct_output(double position,Pair& result);
