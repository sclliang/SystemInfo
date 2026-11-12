using System;
using System.Windows;
using System.Windows.Controls;

namespace SystemInfo.Controls
{
    // Preserve reading order on a small window while giving each column its
    // own vertical flow on desktop. No controls or selections are recreated.
    internal sealed class AdaptiveSheet : Panel
    {
        private const double Gap = 30;
        private bool Wide(double width) => width >= 830;
        private int Column(UIElement child) => (child as FrameworkElement)?.Tag is int column ? column : 0;

        protected override Size MeasureOverride(Size available)
        {
            double width = double.IsInfinity(available.Width) ? 1100 : available.Width;
            bool wide = Wide(width);
            var heights = new double[2];
            foreach (UIElement child in InternalChildren)
            {
                int column = wide ? Column(child) : 0;
                double childWidth = wide ? (width - Gap) * (column == 0 ? .56 : .44) : width;
                child.Measure(new Size(Math.Max(0, childWidth), double.PositiveInfinity));
                heights[column] += child.DesiredSize.Height + Gap;
            }
            return new Size(width, Math.Max(0, Math.Max(heights[0], heights[1]) - Gap));
        }

        protected override Size ArrangeOverride(Size final)
        {
            bool wide = Wide(final.Width);
            var tops = new double[2];
            foreach (UIElement child in InternalChildren)
            {
                int column = wide ? Column(child) : 0;
                double width = wide ? (final.Width - Gap) * (column == 0 ? .56 : .44) : final.Width;
                double left = column == 0 ? 0 : (final.Width - Gap) * .56 + Gap;
                child.Arrange(new Rect(left, tops[column], Math.Max(0, width), child.DesiredSize.Height));
                tops[column] += child.DesiredSize.Height + Gap;
            }
            return final;
        }
    }
}
