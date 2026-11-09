# jobs-width-record state (slot03)
Item1 DONE in tree (uncommitted): job_system.cpp legacy pool+parallelForRange deleted, frameJobsInit sizes mc2::jobs pool clamp(hw/2,2,6)/MC2_FRAME_JOBS_WORKERS;
 MC2_FRAME_JOBS now DefOn (kill-switch; ships ON) in Gates/gate_rows_1.inc, gate_table.json, gate_index.gen.h; host_lease.frame_jobs_workers + nofocus.spawn pass MC2_FRAME_JOBS_WORKERS (game cores-1);
 tests/unit/test_job_system_harness.cpp edited (default_pool_width test, startup test no parallelForRange).
Item2 plan: GameOS/gameos/vk_record_pools.h (RecordPools<Cb> core + ParticipantRegistry), VulkanContext::acquireRecordCommandBuffer + per-slot pools, reset in beginFrame, release in destroy;
 FrameRing::alloc mutex; DescPoolChain::allocate mutex; doctest tests/unit/test_record_pools.cpp (register in CMake harness list).
Proof: box_run build unit lint smoke-lite determinism:mc2_10 determinism:mc2_24; job_system_harness at 1/3/7; Windows ninja_build.
