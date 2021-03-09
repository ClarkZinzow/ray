// Copyright 2017 The Ray Authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//  http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#pragma once

// clang-format off
#include "ray/rpc/grpc_client.h"
#include "ray/rpc/node_manager/node_manager_server.h"
#include "ray/rpc/node_manager/node_manager_client.h"
#include "ray/common/id.h"
#include "ray/common/task/task.h"
#include "ray/common/ray_object.h"
#include "ray/common/client_connection.h"
#include "ray/common/task/task_common.h"
#include "ray/common/task/scheduling_resources.h"
#include "ray/object_manager/object_manager.h"
#include "ray/raylet/agent_manager.h"
#include "ray/raylet_client/raylet_client.h"
#include "ray/raylet/local_object_manager.h"
#include "ray/raylet/scheduling/scheduling_ids.h"
#include "ray/raylet/scheduling/cluster_resource_scheduler.h"
#include "ray/raylet/scheduling/cluster_task_manager.h"
#include "ray/raylet/scheduling/cluster_task_manager_interface.h"
#include "ray/raylet/dependency_manager.h"
#include "ray/raylet/worker_pool.h"
#include "ray/rpc/worker/core_worker_client_pool.h"
#include "ray/util/ordered_set.h"
#include "ray/common/asio/instrumented_io_context.h"
#include "ray/common/bundle_spec.h"
#include "ray/raylet/placement_group_resource_manager.h"
// clang-format on

namespace ray {

namespace raylet {

using rpc::ActorTableData;
using rpc::ErrorType;
using rpc::GcsNodeInfo;
using rpc::HeartbeatTableData;
using rpc::JobTableData;
using rpc::ResourceUsageBatchData;

struct NodeManagerConfig {
  /// The node's resource configuration.
  ResourceSet resource_config;
  /// The IP address this node manager is running on.
  std::string node_manager_address;
  /// The port to use for listening to incoming connections. If this is 0 then
  /// the node manager will choose its own port.
  int node_manager_port;
  /// The lowest port number that workers started will bind on.
  /// If this is set to 0, workers will bind on random ports.
  int min_worker_port;
  /// The highest port number that workers started will bind on.
  /// If this is not set to 0, min_worker_port must also not be set to 0.
  int max_worker_port;
  /// An explicit list of open ports that workers started will bind
  /// on. This takes precedence over min_worker_port and max_worker_port.
  std::vector<int> worker_ports;
  /// The soft limit of the number of workers.
  int num_workers_soft_limit;
  /// Number of initial Python workers for the first job.
  int num_initial_python_workers_for_first_job;
  /// The maximum number of workers that can be started concurrently by a
  /// worker pool.
  int maximum_startup_concurrency;
  /// The commands used to start the worker process, grouped by language.
  WorkerCommandMap worker_commands;
  /// The command used to start agent.
  std::string agent_command;
  /// The time between reports resources in milliseconds.
  uint64_t report_resources_period_ms;
  /// Whether to enable fair queueing between task classes in raylet.
  bool fair_queueing_enabled;
  /// Whether to enable automatic object deletion for object spilling.
  bool automatic_object_deletion_enabled;
  /// The store socket name.
  std::string store_socket_name;
  /// The path to the ray temp dir.
  std::string temp_dir;
  /// The path of this ray session dir.
  std::string session_dir;
  /// The raylet config list of this node.
  std::string raylet_config;
  // The time between record metrics in milliseconds, or 0 to disable.
  uint64_t record_metrics_period_ms;
  // The number if max io workers.
  int max_io_workers;
  // The minimum object size that can be spilled by each spill operation.
  int64_t min_spilling_size;
};

class HeartbeatSender {
 public:
  /// Create a heartbeat sender.
  ///
  /// \param self_node_id ID of this node.
  /// \param gcs_client GCS client to send heartbeat.
  HeartbeatSender(NodeID self_node_id, std::shared_ptr<gcs::GcsClient> gcs_client);

  ~HeartbeatSender();

 private:
  /// Send heartbeats to the GCS.
  void Heartbeat();

  /// ID of this node.
  NodeID self_node_id_;
  /// A client connection to the GCS.
  std::shared_ptr<gcs::GcsClient> gcs_client_;
  /// The io service used in heartbeat loop in case of it being
  /// blocked by main thread.
  instrumented_io_context heartbeat_io_service_;
  /// Heartbeat thread, using with heartbeat_io_service_.
  std::unique_ptr<std::thread> heartbeat_thread_;
  std::unique_ptr<PeriodicalRunner> heartbeat_runner_;
  /// The time that the last heartbeat was sent at. Used to make sure we are
  /// keeping up with heartbeats.
  uint64_t last_heartbeat_at_ms_;
};

class NodeManager : public rpc::NodeManagerServiceHandler {
 public:
  /// Create a node manager.
  ///
  /// \param resource_config The initial set of node resources.
  /// \param object_manager A reference to the local object manager.
  NodeManager(instrumented_io_context &io_service, const NodeID &self_node_id,
              const NodeManagerConfig &config, ObjectManager &object_manager,
              std::shared_ptr<gcs::GcsClient> gcs_client,
              std::shared_ptr<ObjectDirectoryInterface> object_directory_,
              std::function<bool(const ObjectID &)> is_plasma_object_spillable);

  /// Process a new client connection.
  ///
  /// \param client The client to process.
  /// \return Void.
  void ProcessNewClient(ClientConnection &client);

  /// Process a message from a client. This method is responsible for
  /// explicitly listening for more messages from the client if the client is
  /// still alive.
  ///
  /// \param client The client that sent the message.
  /// \param message_type The message type (e.g., a flatbuffer enum).
  /// \param message_data A pointer to the message data.
  /// \return Void.
  void ProcessClientMessage(const std::shared_ptr<ClientConnection> &client,
                            int64_t message_type, const uint8_t *message_data);

  /// Subscribe to the relevant GCS tables and set up handlers.
  ///
  /// \return Status indicating whether this was done successfully or not.
  ray::Status RegisterGcs();

  /// Get initial node manager configuration.
  const NodeManagerConfig &GetInitialConfig() const;

  /// Returns debug string for class.
  ///
  /// \return string.
  std::string DebugString() const;

  /// Record metrics.
  void RecordMetrics();

  /// Get the port of the node manager rpc server.
  int GetServerPort() const { return node_manager_server_.GetPort(); }

  LocalObjectManager &GetLocalObjectManager() { return local_object_manager_; }

  /// Trigger global GC across the cluster to free up references to actors or
  /// object ids.
  void TriggerGlobalGC();

  /// Mark the specified objects as failed with the given error type.
  ///
  /// \param error_type The type of the error that caused this task to fail.
  /// \param object_ids The object ids to store error messages into.
  /// \param job_id The optional job to push errors to if the writes fail.
  void MarkObjectsAsFailed(const ErrorType &error_type,
                           const std::vector<rpc::ObjectReference> object_ids,
                           const JobID &job_id);

  /// Stop this node manager.
  void Stop();

 private:
  /// Methods for handling nodes.

  /// Handle an unexpected failure notification from GCS pubsub.
  ///
  /// \param data The data of the worker that died.
  void HandleUnexpectedWorkerFailure(const rpc::WorkerDeltaData &data);

  /// Handler for the addition of a new node.
  ///
  /// \param data Data associated with the new node.
  /// \return Void.
  void NodeAdded(const GcsNodeInfo &data);

  /// Handler for the removal of a GCS node.
  /// \param node_id Id of the removed node.
  /// \return Void.
  void NodeRemoved(const NodeID &node_id);

  /// Handler for the addition or updation of a resource in the GCS
  /// \param node_id ID of the node that created or updated resources.
  /// \param createUpdatedResources Created or updated resources.
  /// \return Void.
  void ResourceCreateUpdated(const NodeID &node_id,
                             const ResourceSet &createUpdatedResources);

  /// Handler for the deletion of a resource in the GCS
  /// \param node_id ID of the node that deleted resources.
  /// \param resource_names Names of deleted resources.
  /// \return Void.
  void ResourceDeleted(const NodeID &node_id,
                       const std::vector<std::string> &resource_names);

  /// Send heartbeats to the GCS.
  void Heartbeat();

  /// Evaluates the local infeasible queue to check if any tasks can be scheduled.
  /// This is called whenever there's an update to the resources on the local node.
  /// \return Void.
  void TryLocalInfeasibleTaskScheduling();

  /// Report resource usage to the GCS.
  void ReportResourceUsage();

  /// Write out debug state to a file.
  void DumpDebugState() const;

  /// Flush objects that are out of scope in the application. This will attempt
  /// to eagerly evict all plasma copies of the object from the cluster.
  void FlushObjectsToFree();

  /// Get profiling information from the object manager and push it to the GCS.
  ///
  /// \return Void.
  void GetObjectManagerProfileInfo();

  /// Handler for a resource usage notification from the GCS.
  ///
  /// \param id The ID of the node manager that sent the resources data.
  /// \param data The resources data including load information.
  /// \return Void.
  void UpdateResourceUsage(const NodeID &id, const rpc::ResourcesData &data);

  /// Handler for a resource usage batch notification from the GCS
  ///
  /// \param resource_usage_batch The batch of resource usage data.
  void ResourceUsageBatchReceived(const ResourceUsageBatchData &resource_usage_batch);

  /// Handle a worker finishing its assigned task.
  ///
  /// \param worker The worker that finished the task.
  /// \return Whether the worker should be returned to the idle pool. This is
  /// only false for direct actor creation calls, which should never be
  /// returned to idle.
  bool FinishAssignedTask(const std::shared_ptr<WorkerInterface> &worker_ptr);

  /// Helper function to produce actor table data for a newly created actor.
  ///
  /// \param task_spec Task specification of the actor creation task that created the
  /// actor.
  /// \param worker The port that the actor is listening on.
  std::shared_ptr<ActorTableData> CreateActorTableDataFromCreationTask(
      const TaskSpecification &task_spec, int port, const WorkerID &worker_id);
  /// Handle a worker finishing an assigned actor creation task.
  /// \param worker The worker that finished the task.
  /// \param task The actor task or actor creation task.
  /// \return Void.
  void FinishAssignedActorCreationTask(WorkerInterface &worker, const Task &task);

  /// Handle blocking gets of objects. This could be a task assigned to a worker,
  /// an out-of-band task (e.g., a thread created by the application), or a
  /// driver task. This can be triggered when a client starts a get call or a
  /// wait call.
  ///
  /// \param client The client that is executing the blocked task.
  /// \param required_object_refs The objects that the client is blocked waiting for.
  /// \param current_task_id The task that is blocked.
  /// \param ray_get Whether the task is blocked in a `ray.get` call.
  /// \param mark_worker_blocked Whether to mark the worker as blocked. This
  ///                            should be False for direct calls.
  /// \return Void.
  void AsyncResolveObjects(const std::shared_ptr<ClientConnection> &client,
                           const std::vector<rpc::ObjectReference> &required_object_refs,
                           const TaskID &current_task_id, bool ray_get,
                           bool mark_worker_blocked);

  /// Handle end of a blocking object get. This could be a task assigned to a
  /// worker, an out-of-band task (e.g., a thread created by the application),
  /// or a driver task. This can be triggered when a client finishes a get call
  /// or a wait call. The given task must be blocked, via a previous call to
  /// AsyncResolveObjects.
  ///
  /// \param client The client that is executing the unblocked task.
  /// \param current_task_id The task that is unblocked.
  /// \param worker_was_blocked Whether we previously marked the worker as
  ///                           blocked in AsyncResolveObjects().
  /// \return Void.
  void AsyncResolveObjectsFinish(const std::shared_ptr<ClientConnection> &client,
                                 const TaskID &current_task_id, bool was_blocked);

  /// Handle a direct call task that is blocked. Note that this callback may
  /// arrive after the worker lease has been returned to the node manager.
  ///
  /// \param worker Shared ptr to the worker, or nullptr if lost.
  void HandleDirectCallTaskBlocked(const std::shared_ptr<WorkerInterface> &worker,
                                   bool release_resources);

  /// Handle a direct call task that is unblocked. Note that this callback may
  /// arrive after the worker lease has been returned to the node manager.
  /// However, it is guaranteed to arrive after DirectCallTaskBlocked.
  ///
  /// \param worker Shared ptr to the worker, or nullptr if lost.
  void HandleDirectCallTaskUnblocked(const std::shared_ptr<WorkerInterface> &worker);

  /// Kill a worker.
  ///
  /// \param worker The worker to kill.
  /// \return Void.
  void KillWorker(std::shared_ptr<WorkerInterface> worker);

  /// Destroy a worker.
  /// We will disconnect the worker connection first and then kill the worker.
  ///
  /// \param worker The worker to destroy.
  /// \return Void.
  void DestroyWorker(
      std::shared_ptr<WorkerInterface> worker,
      rpc::WorkerExitType disconnect_type = rpc::WorkerExitType::SYSTEM_ERROR_EXIT);

  /// When a job finished, loop over all of the queued tasks for that job and
  /// treat them as failed.
  ///
  /// \param job_id The job that exited.
  /// \return Void.
  void CleanUpTasksForFinishedJob(const JobID &job_id);

  /// Handle an object becoming local. This updates any local accounting, but
  /// does not write to any global accounting in the GCS.
  ///
  /// \param object_id The object that is locally available.
  /// \return Void.
  void HandleObjectLocal(const ObjectID &object_id);
  /// Handle an object that is no longer local. This updates any local
  /// accounting, but does not write to any global accounting in the GCS.
  ///
  /// \param object_id The object that has been evicted locally.
  /// \return Void.
  void HandleObjectMissing(const ObjectID &object_id);

  /// Handles the event that a job is started.
  ///
  /// \param job_id ID of the started job.
  /// \param job_data Data associated with the started job.
  /// \return Void
  void HandleJobStarted(const JobID &job_id, const JobTableData &job_data);

  /// Handles the event that a job is finished.
  ///
  /// \param job_id ID of the finished job.
  /// \param job_data Data associated with the finished job.
  /// \return Void.
  void HandleJobFinished(const JobID &job_id, const JobTableData &job_data);

  /// Process client message of SubmitTask
  ///
  /// \param message_data A pointer to the message data.
  /// \return Void.
  void ProcessSubmitTaskMessage(const uint8_t *message_data);

  /// Process client message of NotifyDirectCallTaskBlocked
  ///
  /// \param message_data A pointer to the message data.
  /// \return Void.
  void ProcessDirectCallTaskBlocked(const std::shared_ptr<ClientConnection> &client,
                                    const uint8_t *message_data);

  /// Process client message of RegisterClientRequest
  ///
  /// \param client The client that sent the message.
  /// \param message_data A pointer to the message data.
  /// \return Void.
  void ProcessRegisterClientRequestMessage(
      const std::shared_ptr<ClientConnection> &client, const uint8_t *message_data);

  /// Process client message of AnnounceWorkerPort
  ///
  /// \param client The client that sent the message.
  /// \param message_data A pointer to the message data.
  /// \return Void.
  void ProcessAnnounceWorkerPortMessage(const std::shared_ptr<ClientConnection> &client,
                                        const uint8_t *message_data);

  /// Handle the case that a worker is available.
  ///
  /// \param client The connection for the worker.
  /// \return Void.
  void HandleWorkerAvailable(const std::shared_ptr<ClientConnection> &client);

  /// Handle the case that a worker is available.
  ///
  /// \param worker The pointer to the worker
  /// \return Void.
  void HandleWorkerAvailable(const std::shared_ptr<WorkerInterface> &worker);

  /// Handle a client that has disconnected. This can be called multiple times
  /// on the same client because this is triggered both when a client
  /// disconnects and when the node manager fails to write a message to the
  /// client.
  ///
  /// \param client The client that sent the message.
  /// \param message_data A pointer to the message data.
  /// \return Void.
  void ProcessDisconnectClientMessage(const std::shared_ptr<ClientConnection> &client,
                                      const uint8_t *message_data);

  /// Process client message of FetchOrReconstruct
  ///
  /// \param client The client that sent the message.
  /// \param message_data A pointer to the message data.
  /// \return Void.
  void ProcessFetchOrReconstructMessage(const std::shared_ptr<ClientConnection> &client,
                                        const uint8_t *message_data);

  /// Process client message of WaitRequest
  ///
  /// \param client The client that sent the message.
  /// \param message_data A pointer to the message data.
  /// \return Void.
  void ProcessWaitRequestMessage(const std::shared_ptr<ClientConnection> &client,
                                 const uint8_t *message_data);

  /// Process client message of WaitForDirectActorCallArgsRequest
  ///
  /// \param client The client that sent the message.
  /// \param message_data A pointer to the message data.
  /// \return Void.
  void ProcessWaitForDirectActorCallArgsRequestMessage(
      const std::shared_ptr<ClientConnection> &client, const uint8_t *message_data);

  /// Process client message of PushErrorRequest
  ///
  /// \param message_data A pointer to the message data.
  /// \return Void.
  void ProcessPushErrorRequestMessage(const uint8_t *message_data);

  /// Process worker subscribing to a given plasma object become available. This handler
  /// makes sure that the plasma object is local and calls core worker's PlasmaObjectReady
  /// gRPC endpoint.
  ///
  /// \param client The client that sent the message.
  /// \param message_data A pointer to the message data.
  /// \return void.
  void ProcessSubscribePlasmaReady(const std::shared_ptr<ClientConnection> &client,
                                   const uint8_t *message_data);

  /// Setup callback with Object Manager.
  ///
  /// \return Status indicating whether setup was successful.
  ray::Status SetupPlasmaSubscription();

  /// Handle a `PrepareBundleResources` request.
  void HandlePrepareBundleResources(const rpc::PrepareBundleResourcesRequest &request,
                                    rpc::PrepareBundleResourcesReply *reply,
                                    rpc::SendReplyCallback send_reply_callback) override;

  /// Handle a `CommitBundleResources` request.
  void HandleCommitBundleResources(const rpc::CommitBundleResourcesRequest &request,
                                   rpc::CommitBundleResourcesReply *reply,
                                   rpc::SendReplyCallback send_reply_callback) override;

  /// Handle a `ResourcesReturn` request.
  void HandleCancelResourceReserve(const rpc::CancelResourceReserveRequest &request,
                                   rpc::CancelResourceReserveReply *reply,
                                   rpc::SendReplyCallback send_reply_callback) override;

  /// Handle a `WorkerLease` request.
  void HandleRequestWorkerLease(const rpc::RequestWorkerLeaseRequest &request,
                                rpc::RequestWorkerLeaseReply *reply,
                                rpc::SendReplyCallback send_reply_callback) override;

  /// Handle a `ReturnWorker` request.
  void HandleReturnWorker(const rpc::ReturnWorkerRequest &request,
                          rpc::ReturnWorkerReply *reply,
                          rpc::SendReplyCallback send_reply_callback) override;

  /// Handle a `ReleaseUnusedWorkers` request.
  void HandleReleaseUnusedWorkers(const rpc::ReleaseUnusedWorkersRequest &request,
                                  rpc::ReleaseUnusedWorkersReply *reply,
                                  rpc::SendReplyCallback send_reply_callback) override;

  /// Handle a `ReturnWorker` request.
  void HandleCancelWorkerLease(const rpc::CancelWorkerLeaseRequest &request,
                               rpc::CancelWorkerLeaseReply *reply,
                               rpc::SendReplyCallback send_reply_callback) override;

  /// Handle a `PinObjectIDs` request.
  void HandlePinObjectIDs(const rpc::PinObjectIDsRequest &request,
                          rpc::PinObjectIDsReply *reply,
                          rpc::SendReplyCallback send_reply_callback) override;

  /// Handle a `NodeStats` request.
  void HandleGetNodeStats(const rpc::GetNodeStatsRequest &request,
                          rpc::GetNodeStatsReply *reply,
                          rpc::SendReplyCallback send_reply_callback) override;

  /// Handle a `GlobalGC` request.
  void HandleGlobalGC(const rpc::GlobalGCRequest &request, rpc::GlobalGCReply *reply,
                      rpc::SendReplyCallback send_reply_callback) override;

  /// Handle a `FormatGlobalMemoryInfo`` request.
  void HandleFormatGlobalMemoryInfo(const rpc::FormatGlobalMemoryInfoRequest &request,
                                    rpc::FormatGlobalMemoryInfoReply *reply,
                                    rpc::SendReplyCallback send_reply_callback) override;

  /// Handle a `RequestObjectSpillage` request.
  void HandleRequestObjectSpillage(const rpc::RequestObjectSpillageRequest &request,
                                   rpc::RequestObjectSpillageReply *reply,
                                   rpc::SendReplyCallback send_reply_callback) override;

  /// Handle a `RestoreSpilledObject` request.
  void HandleRestoreSpilledObject(const rpc::RestoreSpilledObjectRequest &request,
                                  rpc::RestoreSpilledObjectReply *reply,
                                  rpc::SendReplyCallback send_reply_callback) override;

  /// Handle a `ReleaseUnusedBundles` request.
  void HandleReleaseUnusedBundles(const rpc::ReleaseUnusedBundlesRequest &request,
                                  rpc::ReleaseUnusedBundlesReply *reply,
                                  rpc::SendReplyCallback send_reply_callback) override;

  /// Handle a `GetSystemConfig` request.
  void HandleGetSystemConfig(const rpc::GetSystemConfigRequest &request,
                             rpc::GetSystemConfigReply *reply,
                             rpc::SendReplyCallback send_reply_callback) override;

  /// Trigger local GC on each worker of this raylet.
  void DoLocalGC();

  /// Push an error to the driver if this node is full of actors and so we are
  /// unable to schedule new tasks or actors at all.
  void WarnResourceDeadlock();

  /// Dispatch tasks to available workers.
  void DispatchScheduledTasksToWorkers();

  /// Whether a task is an actor creation task.
  bool IsActorCreationTask(const TaskID &task_id);

  /// Return back all the bundle resource.
  ///
  /// \param bundle_spec: Specification of bundle whose resources will be returned.
  /// \return Whether the resource is returned successfully.
  bool ReturnBundleResources(const BundleSpecification &bundle_spec);

  /// Publish the infeasible task error to GCS so that drivers can subscribe to it and
  /// print.
  ///
  /// \param task Task that is infeasible
  void PublishInfeasibleTaskError(const Task &task) const;

  /// Send a object restoration request to a remote node of a given node id.
  void SendSpilledObjectRestorationRequestToRemoteNode(const ObjectID &object_id,
                                                       const std::string &spilled_url,
                                                       const NodeID &node_id);

  /// Get pointers to objects stored in plasma. They will be
  /// released once the returned references go out of scope.
  ///
  /// \param[in] object_ids The objects to get.
  /// \param[out] results The pointers to objects stored in
  /// plasma.
  /// \return Whether the request was successful.
  bool GetObjectsFromPlasma(const std::vector<ObjectID> &object_ids,
                            std::vector<std::unique_ptr<RayObject>> *results);

  /// Disconnect a client.
  ///
  /// \param client The client that sent the message.
  /// \param disconnect_type The reason to disconnect the specified client.
  /// \return Void.
  void DisconnectClient(
      const std::shared_ptr<ClientConnection> &client,
      rpc::WorkerExitType disconnect_type = rpc::WorkerExitType::SYSTEM_ERROR_EXIT);

  /// ID of this node.
  NodeID self_node_id_;
  instrumented_io_context &io_service_;
  /// Class to send heartbeat to GCS.
  std::unique_ptr<HeartbeatSender> heartbeat_sender_;
  ObjectManager &object_manager_;
  /// A Plasma object store client. This is used for creating new objects in
  /// the object store (e.g., for actor tasks that can't be run because the
  /// actor died) and to pin objects that are in scope in the cluster.
  plasma::PlasmaClient store_client_;
  /// A client connection to the GCS.
  std::shared_ptr<gcs::GcsClient> gcs_client_;
  /// The object table. This is shared with the object manager.
  std::shared_ptr<ObjectDirectoryInterface> object_directory_;
  /// The runner to run function periodically.
  PeriodicalRunner periodical_runner_;
  /// The period used for the resources report timer.
  uint64_t report_resources_period_ms_;
  /// Whether to enable fair queueing between task classes in raylet.
  bool fair_queueing_enabled_;
  /// Incremented each time we encounter a potential resource deadlock condition.
  /// This is reset to zero when the condition is cleared.
  int resource_deadlock_warned_ = 0;
  /// Whether we have recorded any metrics yet.
  bool recorded_metrics_ = false;
  /// The path to the ray temp dir.
  std::string temp_dir_;
  /// Initial node manager configuration.
  const NodeManagerConfig initial_config_;

  /// A pool of workers.
  WorkerPool worker_pool_;
  /// A manager to resolve objects needed by queued tasks and workers that
  /// called `ray.get` or `ray.wait`.
  DependencyManager dependency_manager_;

  std::unique_ptr<AgentManager> agent_manager_;

  /// The RPC server.
  rpc::GrpcServer node_manager_server_;

  /// The node manager RPC service.
  rpc::NodeManagerGrpcService node_manager_service_;

  /// The agent manager RPC service.
  std::unique_ptr<rpc::AgentManagerServiceHandler> agent_manager_service_handler_;
  rpc::AgentManagerGrpcService agent_manager_service_;

  /// The `ClientCallManager` object that is shared by all `NodeManagerClient`s
  /// as well as all `CoreWorkerClient`s.
  rpc::ClientCallManager client_call_manager_;

  /// Pool of RPC client connections to core workers.
  rpc::CoreWorkerClientPool worker_rpc_pool_;

  /// Manages all local objects that are pinned (primary
  /// copies), freed, and/or spilled.
  LocalObjectManager local_object_manager_;

  /// Map from node ids to addresses of the remote node managers.
  absl::flat_hash_map<NodeID, std::pair<std::string, int32_t>>
      remote_node_manager_addresses_;

  /// Map of workers leased out to direct call clients.
  std::unordered_map<WorkerID, std::shared_ptr<WorkerInterface>> leased_workers_;

  /// Map from owner worker ID to a list of worker IDs that the owner has a
  /// lease on.
  absl::flat_hash_map<WorkerID, std::vector<WorkerID>> leased_workers_by_owner_;

  /// Whether to trigger global GC in the next resource usage report. This will broadcast
  /// a global GC message to all raylets except for this one.
  bool should_global_gc_ = false;

  /// Whether to trigger local GC in the next resource usage report. This will trigger gc
  /// on all local workers of this raylet.
  bool should_local_gc_ = false;

  /// The last time local gc was run.
  int64_t last_local_gc_ns_ = 0;

  /// The interval in nanoseconds between local GC automatic triggers.
  const int64_t local_gc_interval_ns_;

  /// The min interval in nanoseconds between local GC runs (auto + memory pressure
  /// triggered).
  const int64_t local_gc_min_interval_ns_;

  /// These two classes make up the new scheduler. ClusterResourceScheduler is
  /// responsible for maintaining a view of the cluster state w.r.t resource
  /// usage. ClusterTaskManager is responsible for queuing, spilling back, and
  /// dispatching tasks.
  std::shared_ptr<ClusterResourceSchedulerInterface> cluster_resource_scheduler_;
  std::shared_ptr<ClusterTaskManagerInterface> cluster_task_manager_;

  absl::flat_hash_map<ObjectID, std::unique_ptr<RayObject>> pinned_objects_;

  // TODO(swang): Evict entries from these caches.
  /// Cache for the WorkerTable in the GCS.
  absl::flat_hash_set<WorkerID> failed_workers_cache_;
  /// Cache for the NodeTable in the GCS.
  absl::flat_hash_set<NodeID> failed_nodes_cache_;

  /// Concurrency for the following map
  mutable absl::Mutex plasma_object_notification_lock_;

  /// Keeps track of workers waiting for objects
  absl::flat_hash_map<ObjectID, absl::flat_hash_set<std::shared_ptr<WorkerInterface>>>
      async_plasma_objects_notification_ GUARDED_BY(plasma_object_notification_lock_);

  /// Fields that are used to report metrics.
  /// The period between debug state dumps.
  uint64_t record_metrics_period_ms_;

  /// Last time metrics are recorded.
  uint64_t metrics_last_recorded_time_ms_;

  /// Number of tasks that are received and scheduled.
  uint64_t metrics_num_task_scheduled_;

  /// Number of tasks that are executed at this node.
  uint64_t metrics_num_task_executed_;

  /// Number of tasks that are spilled back to other nodes.
  uint64_t metrics_num_task_spilled_back_;

  /// Managers all bundle-related operations.
  std::shared_ptr<PlacementGroupResourceManager> placement_group_resource_manager_;
};

}  // namespace raylet

}  // end namespace ray
