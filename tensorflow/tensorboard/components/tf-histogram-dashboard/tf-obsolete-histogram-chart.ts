/* Copyright 2015 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/
/* tslint:disable:no-namespace variable-name */

module TF {
  export class HistogramChart {
    protected dataFn: VZ.ChartHelpers.DataFn;
    protected tag: string;
    private run2datasets: {[run: string]: Plottable.Dataset};
    protected runs: string[];

    protected xAccessor: Plottable.Accessor<number|Date>;
    protected xScale: Plottable.QuantitativeScale<number|Date>;
    protected yScale: Plottable.QuantitativeScale<number>;
    protected gridlines: Plottable.Components.Gridlines;
    protected center: Plottable.Components.Group;
    protected xAxis: Plottable.Axes.Numeric|Plottable.Axes.Time;
    protected yAxis: Plottable.Axes.Numeric;
    protected xLabel: Plottable.Components.AxisLabel;
    protected yLabel: Plottable.Components.AxisLabel;
    protected outer: Plottable.Components.Table;
    protected colorScale: Plottable.Scales.Color;
    protected tooltip: d3.Selection<any>;
    private plots: Plottable.XYPlot<number|Date, number>[];

    constructor(
        tag: string, dataFn: VZ.ChartHelpers.DataFn, xType: string,
        colorScale: Plottable.Scales.Color, tooltip: d3.Selection<any>) {
      this.dataFn = dataFn;
      this.run2datasets = {};
      this.tag = tag;
      this.colorScale = colorScale;
      this.tooltip = tooltip;
      this.buildChart(xType);
    }

    /**
     * Change the runs on the chart. The work of actually setting the dataset
     * on the plot is deferred to the subclass because it is impl-specific.
     * Changing runs automatically triggers a reload; this ensures that the
     * newly selected run will have data, and that all the runs will be current
     * (it would be weird if one run was ahead of the others, and the display
     * depended on the order in which runs were added)
     */
    public changeRuns(runs: string[]) {
      this.runs = runs;
      this.reload();
      let datasets = runs.map((r) => this.getDataset(r));
      this.plots.forEach((p) => p.datasets(datasets));
    }

    /**
     * Reload data for each run in view.
     */
    public reload() {
      this.runs.forEach((run) => {
        let dataset = this.getDataset(run);
        this.dataFn(this.tag, run).then((x) => dataset.data(x));
      });
    }

    protected getDataset(run: string) {
      if (this.run2datasets[run] === undefined) {
        this.run2datasets[run] =
            new Plottable.Dataset([], {run: run, tag: this.tag});
      }
      return this.run2datasets[run];
    }

    protected buildChart(xType: string) {
      if (this.outer) {
        this.outer.destroy();
      }
      let xComponents = VZ.ChartHelpers.getXComponents(xType);
      this.xAccessor = xComponents.accessor;
      this.xScale = xComponents.scale;
      this.xAxis = xComponents.axis;
      this.xAxis.margin(0).tickLabelPadding(3);
      this.yScale = new Plottable.Scales.Linear();
      this.yAxis = new Plottable.Axes.Numeric(this.yScale, 'left');
      let yFormatter = VZ.ChartHelpers.multiscaleFormatter(
          VZ.ChartHelpers.Y_AXIS_FORMATTER_PRECISION);
      this.yAxis.margin(0).tickLabelPadding(5).formatter(yFormatter);
      this.yAxis.usesTextWidthApproximation(true);

      let center = this.buildPlot(this.xAccessor, this.xScale, this.yScale);

      this.gridlines =
          new Plottable.Components.Gridlines(this.xScale, this.yScale);

      this.center = new Plottable.Components.Group([this.gridlines, center]);
      this.outer = new Plottable.Components.Table(
          [[this.yAxis, this.center], [null, this.xAxis]]);
    }

    protected buildPlot(xAccessor, xScale, yScale): Plottable.Component {
      let percents = [0, 228, 1587, 3085, 5000, 6915, 8413, 9772, 10000];
      let opacities = _.range(percents.length - 1)
                          .map((i) => (percents[i + 1] - percents[i]) / 2500);
      let accessors = percents.map((p, i) => (datum) => datum[i][1]);
      let median = 4;
      let medianAccessor = accessors[median];

      let plots = _.range(accessors.length - 1).map((i) => {
        let p = new Plottable.Plots.Area<number|Date>();
        p.x(xAccessor, xScale);

        let y0 = i > median ? accessors[i] : accessors[i + 1];
        let y = i > median ? accessors[i + 1] : accessors[i];
        p.y(y, yScale);
        p.y0(y0);
        p.attr(
            'fill', (d: any, i: number, dataset: Plottable.Dataset) =>
                        this.colorScale.scale(dataset.metadata().run));
        p.attr(
            'stroke', (d: any, i: number, dataset: Plottable.Dataset) =>
                          this.colorScale.scale(dataset.metadata().run));
        p.attr('stroke-weight', (d: any, i: number, m: any) => '0.5px');
        p.attr('stroke-opacity', () => opacities[i]);
        p.attr('fill-opacity', () => opacities[i]);
        return p;
      });

      let medianPlot = new Plottable.Plots.Line<number|Date>();
      medianPlot.x(xAccessor, xScale);
      medianPlot.y(medianAccessor, yScale);
      medianPlot.attr(
          'stroke',
          (d: any, i: number, m: any) => this.colorScale.scale(m.run));

      this.plots = plots;
      return new Plottable.Components.Group(plots);
    }

    public renderTo(target: d3.Selection<any>) { this.outer.renderTo(target); }

    public redraw() { this.outer.redraw(); }

    protected destroy() { this.outer.destroy(); }
  }
}
