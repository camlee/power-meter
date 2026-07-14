import { mount } from 'svelte';
import App from './App.svelte';

const target = document.getElementById('app');
try {
  target.replaceChildren();
  mount(App, { target });
} catch (error) {
  console.error(error);
  target.innerHTML = '<div class="boot"><h1>power-meter</h1><p>The interface could not start. Refresh this page and try again.</p></div>';
}
