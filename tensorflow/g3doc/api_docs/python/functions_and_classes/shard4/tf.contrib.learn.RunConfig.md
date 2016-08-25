This class specifies the specific configurations for the run.

If you're a Google-internal user using command line flags with learn_runner.py
(for instance, to do distributed training or to use parameter servers), you
probably want to use learn_runner.EstimatorConfig instead.
- - -

#### `tf.contrib.learn.RunConfig.__init__(master='', task=0, num_ps_replicas=0, num_cores=4, log_device_placement=False, gpu_memory_fraction=1, cluster_spec=None, tf_random_seed=None, save_summary_steps=100, save_checkpoints_secs=60, keep_checkpoint_max=5, keep_checkpoint_every_n_hours=10000, job_name=None)` {#RunConfig.__init__}

Constructor.

##### Args:


*  <b>`master`</b>: TensorFlow master. Empty string (the default) for local.
*  <b>`task`</b>: Task id of the replica running the training (default: 0).
*  <b>`num_ps_replicas`</b>: Number of parameter server tasks to use (default: 0).
*  <b>`num_cores`</b>: Number of cores to be used (default: 4).
*  <b>`log_device_placement`</b>: Log the op placement to devices (default: False).
*  <b>`gpu_memory_fraction`</b>: Fraction of GPU memory used by the process on
    each GPU uniformly on the same machine.
*  <b>`cluster_spec`</b>: a tf.train.ClusterSpec object that describes the cluster in
    the case of distributed computation. If missing, reasonable assumptions
    are made for the addresses of jobs.
*  <b>`tf_random_seed`</b>: Random seed for TensorFlow initializers.
    Setting this value allows consistency between reruns.
*  <b>`save_summary_steps`</b>: Save summaries every this many steps.
*  <b>`save_checkpoints_secs`</b>: Save checkpoints every this many seconds.
*  <b>`keep_checkpoint_max`</b>: The maximum number of recent checkpoint files to
    keep. As new files are created, older files are deleted. If None or 0,
    all checkpoint files are kept. Defaults to 5 (that is, the 5 most recent
    checkpoint files are kept.)
*  <b>`keep_checkpoint_every_n_hours`</b>: Number of hours between each checkpoint
    to be saved. The default value of 10,000 hours effectively disables
    the feature.
*  <b>`job_name`</b>: the type of task, e.g., 'ps', 'worker', etc. Must exist in
    cluster_spec.jobs.


- - -

#### `tf.contrib.learn.RunConfig.job_name` {#RunConfig.job_name}




