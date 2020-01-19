<script>
  import LinearProgress from '@smui/linear-progress';

  export let action = "";
  export let action_verb = "";
  export let error = "";
</script>

<style>
  .progress-bar {
    width: 200px;
    position: absolute;
    left: 50%;
    top: 50%;
    transform: translate(-50%, -50%);
  }

  .progress-message{
    position: absolute;
    left: 50%;
    top: 50%;
    transform: translate(-50%, 4px);
    text-align: center;
  }
</style>

<div class="progress-bar">
  {#if error}
    <LinearProgress class="error-progress-bar" progress={1}/>
  {:else}
    <LinearProgress indeterminate/>
  {/if}
</div>

<div class="progress-message">
  {#if error}
    <h5>Failed to {action}: {error}</h5>
  {:else}
    <h3>{action_verb}</h3>
  {/if}
</div>